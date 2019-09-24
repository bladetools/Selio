#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>

#include <iostream>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#include "Selio.hpp"
#include "test.h"

using namespace std;
using namespace selio;

typedef std::shared_ptr<UnixSocket<> > UnixSocketPtr;

int quit = 0;

void interrupt(int sig) {
    fprintf(stderr, "Ctrl+C interrupt\n");
    quit = 1;
}

int main(int argc, char const *argv[])
{
    ::signal(SIGINT, interrupt);
    ::signal(SIGTERM, interrupt);

    UnixSocketPtr server = make_shared<UnixSocket<> >();

    if (server->create() < 0) {
        fprintf(stderr, "Unable to create socket %d\n", errno);
        return -1;
    }

    if (server->bind(SOCK_FILE_NAME, SOCK_FILE_NAME_LEN) < 0) {
        fprintf(stderr, "Unable to bind socket %d\n", errno);
        return -1;
    }

    printf("server\n");
    printf("pid: %d\n", getpid());
    printf("fd: %d\n", server->getFd());

    Selector<UnixSocketPtr> selector;
    selector.add(server, SEL_ACCEPT);

    while (!quit) {
        int nfd = selector.select(500);
        if (nfd == 0)
            continue;
        if (nfd < 0) {
            fprintf(stderr, "select error %s\n", strerror(errno));
            return -1;
        }
#if SENDFD
        for (UnixSocketPtr sock : selector.getSelectedFds()) {
            if (sock->isAcceptable()) {
                struct sockaddr_un addr;
                socklen_t addrLen = sizeof(struct sockaddr_un);
                int clifd = ::accept(sock->getFd(), (struct sockaddr*)&addr, &addrLen);
                if (clifd == -1) {
                    fprintf(stderr, "Accept failed\n");
                    continue;
                }
                printf("Accept %s\n", addr.sun_path);
                auto client = make_shared<UnixSocket<> >(clifd);
                selector.add(client, SEL_READ);
            } else if (sock->isReadable()) {
                char buf[64];
                struct msghdr msg = { 0 };
                struct cmsghdr *cmsg;
                struct iovec io {
                    .iov_base = buf,
                    .iov_len = 64
                };

                union {
                    struct cmsghdr hdr;
                    char buf[CMSG_SPACE(sizeof(int) * 1)];
                } u;

                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = u.buf;
                msg.msg_controllen = sizeof(u.buf);

                int ret = sock->recvmsg(&msg);
                if (ret <= 0) {
                    selector.remove(sock);
                    fprintf(stderr, "Receive message failed %d %s\n", ret, strerror(errno));
                    continue;
                }

                cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                    int *fds = (int*)CMSG_DATA(cmsg);
                    if (fds && *fds) {
                        char buf[3];
                        read(*fds, buf, 2);
                        buf[2] = 0;
                        puts(buf);
                    }
                }
            }
        }
#else
        for (UnixSocketPtr sock : selector.getSelectedFds()) {
            if (sock->isAcceptable()) {
                struct sockaddr_un addr;
                socklen_t addrLen = sizeof(struct sockaddr_un);
                int clifd = ::accept(sock->getFd(), (struct sockaddr*)&addr, &addrLen);
                if (clifd == -1) {
                    fprintf(stderr, "Accept failed\n");
                    continue;
                }
                printf("Accept %s\n", addr.sun_path);
                auto client = make_shared<UnixSocket<> >(clifd);
                selector.add(client, SEL_READ);
                client->send("string from server\n", 19);
            } else if (sock->isReadable()) {
                char buf[64];
                ssize_t ret = sock->recv(buf, 64);
                if (ret <= 0) {
                    selector.remove(sock);
                    fprintf(stderr, "Receive failed %zd\n", ret);
                    continue;
                }
                buf[ret] = 0;
                printf("%s\n", buf);
            }
        }
#endif
    }

    return 0;
}
