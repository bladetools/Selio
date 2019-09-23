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

#define SENDFD 1

using namespace std;
using namespace selio;

typedef std::shared_ptr<UnixSocket> UnixSocketPtr;

int quit = 0;

void interrupt(int sig) {
    fprintf(stderr, "Ctrl+C interrupt\n");
    quit = 1;
}

int main(int argc, char const *argv[])
{
    ::signal(SIGINT, interrupt);
    ::signal(SIGTERM, interrupt);

    UnixSocketPtr client = make_shared<UnixSocket>();

    if (client->connect("\0/tmp/test_server.socket") < 0) {
        fprintf(stderr, "Unable to connect socket %s\n", strerror(errno));
        return -1;
    }
    printf("client\n");
    printf("pid: %d\n", getpid());
    printf("fd: %d\n", client->getFd());

#if SENDFD
    int data_fd = open("data.txt", O_RDWR);
    if (data_fd == -1)
        fprintf(stderr, "unable to open data.txt");

    char data_buf[3];
    read(data_fd, data_buf, 2);
    data_buf[2] = 0;
    puts(data_buf);
#endif

    Selector<UnixSocketPtr> selector;
    selector.add(client, SEL_CONNECT);

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
            if (sock->isConnectable() && sock->isConnected()) {
                printf("Connected\n");
                selector.set(sock, SEL_READ);
                
                struct msghdr msg = { 0 };
                struct cmsghdr *cmsg;
                int fds[1] = { data_fd };
                struct iovec io = {
                    .iov_base = (void*)"string from client\n",
                    .iov_len = 19
                };
                union {
                    struct cmsghdr hdr;
                    char buf[CMSG_SPACE(sizeof(fds))];
                } u;
                
                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = u.buf;
                msg.msg_controllen = sizeof(u.buf);
                cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 1);
                memcpy(CMSG_DATA(cmsg), &fds, sizeof(int) * 1);

                int ret = sock->sendmsg(&msg);
                if (ret <= 0) {
                    selector.remove(sock);
                    fprintf(stderr, "Send message failed %d %s\n", ret, strerror(errno));
                }
            }
        }
#else
        for (UnixSocketPtr sock : selector.getSelectedFds()) {
            if (sock->isConnectable() && sock->isConnected()) {
                printf("Connected\n");
                selector.set(sock, SEL_READ);
                sock->send("string from client\n", 18);
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
