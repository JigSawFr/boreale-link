// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <time.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

uint64_t bl_now_ms(void)
{
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static int set_blocking(int fd, bool blocking)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

// One address, with a deadline for the TCP handshake.
static int connect_one(const struct addrinfo *ai, int timeout_ms, char *err, size_t err_size)
{
    const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(err, err_size, "socket: %s", strerror(errno));
        return -1;
    }
    if (set_blocking(fd, false) < 0) {
        snprintf(err, err_size, "socket mode: %s", strerror(errno));
        close(fd);
        return -1;
    }
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        snprintf(err, err_size, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (rc < 0) {
        struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
        rc = poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            snprintf(err, err_size, rc == 0 ? "no answer from the broker" : "poll: %s", strerror(errno));
            close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            snprintf(err, err_size, "connect: %s", strerror(so_error ? so_error : errno));
            close(fd);
            return -1;
        }
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));   // small packets, sent now
    return fd;
}

static int tcp_open(void *ctx, const char *host, uint16_t port, int timeout_ms, char *err, size_t err_size)
{
    BlTcp *t = (BlTcp *)ctx;
    t->fd = -1;
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *list = NULL;
    const int g = getaddrinfo(host, service, &hints, &list);
    if (g != 0 || !list) {
        snprintf(err, err_size, "cannot find %s: %s", host, g ? gai_strerror(g) : "no address");
        return -1;
    }
    int fd = -1;
    for (const struct addrinfo *ai = list; ai && fd < 0; ai = ai->ai_next) fd = connect_one(ai, timeout_ms, err, err_size);
    freeaddrinfo(list);
    if (fd < 0) return -1;
    t->fd = fd;
    return 0;
}

// At most this much per send(): a large document (the games, 64 KiB) goes in
// pieces, so the network service never needs room for all of it at once (a
// sysmodule's buffers are small).
#define SEND_CHUNK 0x2000

// All of it within `timeout_ms`, however the socket answers: one that says
// it can take more and then takes nothing (a connection lost while the
// console slept) gives up at the deadline rather than trying forever.
static int tcp_write(void *ctx, const void *buf, size_t len, int timeout_ms)
{
    BlTcp *t = (BlTcp *)ctx;
    const uint8_t *p = (const uint8_t *)buf;
    const uint64_t deadline = bl_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    while (len > 0) {
        const uint64_t now = bl_now_ms();
        if (now >= deadline) return -1;
        struct pollfd pf = { .fd = t->fd, .events = POLLOUT, .revents = 0 };
        const int r = poll(&pf, 1, (int)(deadline - now));
        if (r <= 0 || (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
        const ssize_t n = send(t->fd, p, len > SEND_CHUNK ? SEND_CHUNK : len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static long tcp_read(void *ctx, void *buf, size_t len, int timeout_ms)
{
    BlTcp *t = (BlTcp *)ctx;
    if (len == 0) return 0;
    struct pollfd pf = { .fd = t->fd, .events = POLLIN, .revents = 0 };
    const int r = poll(&pf, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    if (pf.revents & POLLNVAL) return -1;
    const ssize_t n = recv(t->fd, buf, len, 0);
    if (n == 0) return -1;   // closed by the broker
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    return (long)n;
}

static void tcp_close(void *ctx)
{
    BlTcp *t = (BlTcp *)ctx;
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
}

BlIo bl_tcp_io(BlTcp *tcp)
{
    tcp->fd = -1;
    BlIo io = { tcp, tcp_open, tcp_write, tcp_read, tcp_close };
    return io;
}
