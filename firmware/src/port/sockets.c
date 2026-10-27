/*
 * TCP and UDP for the board layers with BSD sockets: the ESP32's (lwIP under
 * ESP-IDF) and the host build's (Linux). All non-blocking, as port.h asks.
 */
#if !defined(ARDUINO_ARCH_RP2040)

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "port.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* For testing on the host: a send buffer this small (0: the stack's own), and
 * writes held back while sockets_stalled() is true (NULL: never). */
int sockets_sndbuf;
bool (*sockets_stalled)(void);

static bool resolve(const char *host, uint16_t port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = type}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return false;
    memcpy(out, res->ai_addr, sizeof *out);
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

static int open_socket(int type)
{
    int fd = socket(AF_INET, type, 0);
    if (fd >= 0)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

int port_tcp_connect(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_STREAM, &addr))
        return -1;
    int fd = open_socket(SOCK_STREAM);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (sockets_sndbuf)
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sockets_sndbuf, sizeof sockets_sndbuf);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    /* Wait at most 2 s for the connection. */
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {2, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (select(fd + 1, NULL, &w, NULL, &tv) != 1 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        close(fd);
        return -1;
    }
    return fd;
}

int port_tcp_write(int h, const void *buf, size_t len)
{
    if (sockets_stalled && sockets_stalled())
        return 0;
    int n = (int)send(h, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0)
        return n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

int port_tcp_read(int h, void *buf, size_t len)
{
    int n = (int)recv(h, buf, len, MSG_DONTWAIT);
    if (n > 0)
        return n;
    if (n == 0)
        return -1; /* closed by the server */
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

void port_tcp_close(int h)
{
    if (h >= 0)
        close(h);
}

#ifdef BPS_NET_DEBUG
bool port_tcp_get_stats(int h, port_tcp_stats *out)
{
    (void)h;
    (void)out;
    return false; /* the stack's internals are its own */
}
#endif

int port_udp_open(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_DGRAM, &addr))
        return -1;
    int fd = open_socket(SOCK_DGRAM);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int port_udp_send(int h, const void *buf, size_t len)
{
    return send(h, buf, len, MSG_DONTWAIT) == (ssize_t)len ? (int)len : -1;
}

int port_udp_recv(int h, void *buf, size_t len)
{
    int n = (int)recv(h, buf, len, MSG_DONTWAIT);
    if (n >= 0)
        return n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

void port_udp_close(int h)
{
    if (h >= 0)
        close(h);
}

#endif
