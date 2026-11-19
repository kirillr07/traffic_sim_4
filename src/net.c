// net.c: обёртки над сокетами TCP для сервера и клиентов

#include "net.h"
#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int set_reuseaddr(int fd)
{
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

int net_listen(const char *ip, const char *port, int backlog)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char *node = (ip && *ip) ? ip : NULL;
    int rc = getaddrinfo(node, port, &hints, &res);
    if (rc != 0) {
        io_printf(2, "[сеть] getaddrinfo(%s:%s): %s\n",
                  node ? node : "*", port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        set_reuseaddr(fd);
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int net_connect(const char *ip, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(ip, port, &hints, &res);
    if (rc != 0) {
        io_printf(2, "[сеть] getaddrinfo(%s:%s): %s\n",
                  ip, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;

        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

ssize_t net_recv_all(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0)
            return got == 0 ? 0 : -1;

        got += (size_t)n;
    }
    return (ssize_t)got;
}

int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_peer_name(int fd, char *buf, size_t cap)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];

    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
        return -1;
    if (getnameinfo((struct sockaddr *)&ss, slen,
                    host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return -1;

    snprintf(buf, cap, "%s:%s", host, serv);
    return 0;
}

void net_close(int fd)
{
    if (fd >= 0) close(fd);
}
