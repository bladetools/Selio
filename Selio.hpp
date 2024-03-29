#ifndef __Selio_hpp__
#define __Selio_hpp__

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <iostream>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

#define SELIO_DISABLE_COPY_COTOR(Cls) \
    Cls(const Cls &s) = delete; \
    Cls& operator=(const Cls &s) = delete;

namespace selio {

template<class SelectableType>
class Selector;

enum {
    SEL_NONE    = 0,
    SEL_READ    = 1,
    SEL_WRITE   = 2,
    SEL_CONNECT = 4,
    SEL_ACCEPT  = 8,
    SEL_MASK    = 0x0f
};


template<class UDT = void*>
class Selectable {
protected:
    int fd;
    int sel;
    int ready;
public:
    UDT data;

    explicit Selectable(int fd = -1) : fd(fd), ready(false) {  }
    
    virtual ~Selectable() {
        close();
    }

    Selectable(Selectable &&s) {
        fd = s.fd;
        sel = s.sel;
        ready = s.ready;
        s.fd = -1;
        s.sel = 0;
        s.ready = 0;
    }

    SELIO_DISABLE_COPY_COTOR(Selectable);

    int getFd() { return fd; }

    int releaseFd() { int s = fd; fd = -1; return s; }

    int setBlocking(bool block) {
        int ret = fcntl(fd, F_GETFL);
        if (ret == -1)
            return ret;

        int nfl = 0;

        if (block && !(ret & O_NONBLOCK)) {
            nfl = ret | O_NONBLOCK;
        } else if (!block && (ret & O_NONBLOCK)) {
            nfl = ret & ~O_NONBLOCK;
        }

        if (nfl == ret)
            return 0;

        return fcntl(fd, F_SETFL, nfl);
    }

    void close() {
        if (fd != -1)
            ::close(fd);
    }

    ssize_t read(void *data, size_t size) {
        return ::read(fd, data, size);
    }

    ssize_t write(const void *data, size_t size) {
        return ::write(fd, data, size);
    }

    bool isReadable() {
        return SEL_READ & ready;
    }

    bool isWritable() {
        return SEL_WRITE & ready;
    }

    bool isConnectable() {
        return SEL_CONNECT & ready;
    }

    bool isAcceptable() {
        return SEL_ACCEPT & ready;
    }

    template<class SelectableType>
    friend class Selector;
};

template<class UDT = void*>
class SelectableSocket : public Selectable<UDT> {
public:
    explicit SelectableSocket(int fd = -1) : Selectable<UDT>(fd) { }

    SelectableSocket(SelectableSocket &&s) : Selectable<UDT>(std::move(s)) { }

    virtual ~SelectableSocket() { }

    SELIO_DISABLE_COPY_COTOR(SelectableSocket);

    int create() {
        this->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (this->fd == -1)
            return -1;

        return this->fd;
    }

    int setsockopt(int optname, int val) {
        return ::setsockopt(this->fd, SOL_SOCKET, optname, &val, sizeof(val));
    }

    int getsockopt(int optname, int *p) {
        socklen_t errLen = sizeof(int);
        return ::getsockopt(this->fd, SOL_SOCKET, optname, &p, &errLen);
    }

    bool isConnected() {
        int val = 0;
        if (getsockopt(SO_ERROR, &val) < 0)
            return false;
        return !val;
    }

    bool isClosed() {
        return this->fd == -1;
    }

    ssize_t recv(void *data, size_t size, int flags = 0) {
        return ::recv(this->fd, data, size, flags);
    }

    ssize_t send(const void *data, size_t size, int flags = 0) {
        return ::send(this->fd, data, size, flags);
    }

    ssize_t sendmsg(const struct msghdr *msg, int flags = 0) {
        return ::sendmsg(this->fd, msg, flags);
    }

    ssize_t recvmsg(struct msghdr *msg, int flags = 0) {
        return ::recvmsg(this->fd, msg, flags);
    }
};

template<class UDT = void*>
class UnixSocket : public SelectableSocket<UDT>
{
private:
    bool keepFile;
    std::string name;
public:
    explicit UnixSocket(int fd = -1) : SelectableSocket<UDT>(fd), keepFile(false) { }

    UnixSocket(UnixSocket &&s) : SelectableSocket<UDT>(std::move(s)), keepFile(s.keepFile), name(std::move(s.name)) { 
        s.keepFile = false;
    }

    virtual ~UnixSocket() {
        if (!keepFile && name.size() && name[0])
            unlink(name.c_str());
    }

    SELIO_DISABLE_COPY_COTOR(UnixSocket);

    int connect(const char *filename, ssize_t nameLen = -1) {
        int err;

        if (this->fd == -1) {
            errno = EBADF;
            return -1;
        }

        if (filename == nullptr) {
            errno = EINVAL;
            return -1;
        }

        if (nameLen == -1)
            nameLen = strlen(filename);

        keepFile = true;
        name = std::string(filename, nameLen);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        memcpy(&addr.sun_path[0], name.data(), name.size());

        if (::connect(this->fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un) - (sizeof(addr.sun_path) - nameLen)) == -1)
            goto ERROR;

        return this->fd;
ERROR:
        err = errno;
        if (this->fd != -1)
            ::close(this->fd);
        errno = err;
        return -1;
    }

    int bind(const char *filename, ssize_t nameLen = -1, int type = SOCK_STREAM, int backlog = 50) {
        int err;

        if (this->fd == -1) {
            errno = EBADF;
            return -1;
        }

        if (filename == nullptr) {
            errno = EINVAL;
            return -1;
        }

        if (nameLen == -1)
            nameLen = strlen(filename);

        keepFile = false;
        name = std::string(filename, nameLen);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        memcpy(&addr.sun_path[0], name.data(), name.size());

        if (::bind(this->fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un) - (sizeof(addr.sun_path) - nameLen)) == -1)
            goto ERROR;

        if (::listen(this->fd, backlog) == -1)
            goto ERROR;
        
        return this->fd;
ERROR:
        err = errno;
        if (this->fd != -1)
            ::close(this->fd);
        errno = err;
        return -1;
    }
};


template<class SelectableType>
class Selector {
private:
    std::vector<SelectableType> fds;
    std::vector<SelectableType> selectedFds;
public:

    void add(SelectableType s, int sel) {
        s->sel = sel;
        fds.push_back(s);
    }

    void remove(SelectableType s) {
        fds.erase(std::find(fds.begin(), fds.end(), s));
    }

    void set(SelectableType s, int sel) {
        auto it = std::find(fds.begin(), fds.end(), s);
        if (it != fds.end())
            s->sel = sel;
    }

    const std::vector<SelectableType> getFds() { return fds; }

    const std::vector<SelectableType> getSelectedFds() { return selectedFds; }

    int select(long timeout) {
        int nfds = -1;
        struct timeval timeval, *pTimeout = nullptr;
        fd_set r, w;

        if (timeout) {
            timeval.tv_sec = timeout / 1000;
            timeval.tv_usec = (timeout % 1000) * 1000;
            pTimeout = &timeval;
        }
        
        FD_ZERO(&r);
        FD_ZERO(&w);

        for (auto &s : fds) {
            s->ready = 0;

            if (s->sel & SEL_MASK && s->getFd() > nfds)
                nfds = s->getFd();

            if (s->sel & SEL_READ) FD_SET(s->getFd(), &r);
            if (s->sel & SEL_WRITE) FD_SET(s->getFd(), &w);
            if (s->sel & SEL_CONNECT) FD_SET(s->getFd(), &w);
            if (s->sel & SEL_ACCEPT) FD_SET(s->getFd(), &r);
        }

        int ret = ::select(nfds + 1, &r, &w, nullptr, pTimeout);

        if (ret) {
            selectedFds.clear();
            for (auto &s : fds) {
                if (s->sel & SEL_MASK) {
                    selectedFds.push_back(s);
                    if (s->sel & SEL_READ && FD_ISSET(s->getFd(), &r)) s->ready |= SEL_READ;
                    if (s->sel & SEL_WRITE && FD_ISSET(s->getFd(), &w)) s->ready |= SEL_WRITE;
                    if (s->sel & SEL_CONNECT && FD_ISSET(s->getFd(), &w)) s->ready |= SEL_CONNECT;
                    if (s->sel & SEL_ACCEPT && FD_ISSET(s->getFd(), &r)) s->ready |= SEL_ACCEPT;
                }
            }
        }

        return ret;
    }
};

}

#endif // __Selio_hpp__

