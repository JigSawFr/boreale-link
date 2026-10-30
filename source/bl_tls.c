// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_tls.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <errno.h>
#include <fcntl.h>
#include <switch.h>
#include <unistd.h>

_Static_assert(sizeof(SslContext) <= sizeof(((BlTls *)0)->ctx), "SslContext does not fit");
_Static_assert(sizeof(SslConnection) <= sizeof(((BlTls *)0)->conn), "SslConnection does not fit");

#define CTX(t)  ((SslContext *)(void *)(t)->ctx)
#define CONN(t) ((SslConnection *)(void *)(t)->conn)
#define CA_MAX  16384

static void tls_close(void *vctx)
{
    BlTls *t = (BlTls *)vctx;
    if (t->conn_open) sslConnectionClose(CONN(t));
    if (t->ctx_open) sslContextClose(CTX(t));
    t->conn_open = t->ctx_open = false;
    // DoNotCloseSocket: the socket stayed ours.
    if (t->tcp.fd >= 0) close(t->tcp.fd);
    t->tcp.fd = -1;
    if (t->ssl_open) sslExit();
    t->ssl_open = false;
}

static int tls_fail(BlTls *t, char *err, size_t err_size, const char *what, Result rc)
{
    snprintf(err, err_size, "%s (TLS, 0x%08X)", what, (unsigned)rc);
    tls_close(t);
    return -1;
}

static int tls_open(void *vctx, const char *host, uint16_t port, int timeout_ms, char *err, size_t err_size)
{
    BlTls *t = (BlTls *)vctx;
    BlIo tcp = bl_tcp_io(&t->tcp);
    if (tcp.open(&t->tcp, host, port, timeout_ms, err, err_size) < 0) return -1;
    // The ssl service works on a blocking socket.
    const int flags = fcntl(t->tcp.fd, F_GETFL, 0);
    if (flags >= 0) fcntl(t->tcp.fd, F_SETFL, flags & ~O_NONBLOCK);

    Result rc = sslInitialize(1);
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "the console's TLS service is unavailable", rc);
    t->ssl_open = true;
    rc = sslCreateContext(CTX(t), SslVersion_Auto);
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "cannot create a TLS context", rc);
    t->ctx_open = true;
    if (t->ca_file && *t->ca_file) {
        static char pem[CA_MAX];
        FILE *f = fopen(t->ca_file, "rb");
        if (!f) {
            snprintf(err, err_size, "cannot read the certificate %s", t->ca_file);
            tls_close(t);
            return -1;
        }
        const size_t n = fread(pem, 1, sizeof(pem), f);
        fclose(f);
        u64 id = 0;
        rc = n > 0 && n < sizeof(pem) ? sslContextImportServerPki(CTX(t), pem, (u32)n, SslCertificateFormat_Pem, &id)
                                      : MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if (R_FAILED(rc)) return tls_fail(t, err, err_size, "the certificate file was refused", rc);
    }
    rc = sslContextCreateConnection(CTX(t), CONN(t));
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "cannot create a TLS connection", rc);
    t->conn_open = true;
    rc = sslConnectionSetOption(CONN(t), SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "cannot keep the socket", rc);
    errno = 0;
    if (socketSslConnectionSetSocketDescriptor(CONN(t), t->tcp.fd) < 0 && errno != ENOENT)
        return tls_fail(t, err, err_size, "cannot hand the socket to TLS", (Result)errno);
    rc = sslConnectionSetHostName(CONN(t), host, (u32)strlen(host));
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "the broker's name was refused", rc);
    rc = sslConnectionSetIoMode(CONN(t), SslIoMode_Blocking);
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "cannot set the TLS mode", rc);
    rc = sslConnectionDoHandshake(CONN(t), NULL, NULL, NULL, 0);
    if (R_FAILED(rc)) return tls_fail(t, err, err_size, "TLS handshake failed (certificate or name)", rc);
    return 0;
}

static int tls_write(void *vctx, const void *buf, size_t len, int timeout_ms)
{
    BlTls *t = (BlTls *)vctx;
    (void)timeout_ms;
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        u32 n = 0;
        if (R_FAILED(sslConnectionWrite(CONN(t), p, (u32)len, &n)) || n == 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static long tls_read(void *vctx, void *buf, size_t len, int timeout_ms)
{
    BlTls *t = (BlTls *)vctx;
    if (len == 0) return 0;
    s32 pending = 0;
    if (R_FAILED(sslConnectionPending(CONN(t), &pending))) return -1;
    if (pending <= 0) {
        u32 events = 0;
        const Result rc = sslConnectionPoll(CONN(t), SslPollEvent_Read, &events, (u32)(timeout_ms > 0 ? timeout_ms : 0));
        if (R_FAILED(rc)) return -1;
        if (!(events & SslPollEvent_Read)) return 0;
    }
    u32 n = 0;
    if (R_FAILED(sslConnectionRead(CONN(t), buf, (u32)len, &n))) return -1;
    if (n == 0) return -1;   // closed
    return (long)n;
}

BlIo bl_tls_io(BlTls *tls, const char *ca_file)
{
    memset(tls, 0, sizeof(*tls));
    tls->tcp.fd = -1;
    tls->ca_file = ca_file;
    BlIo io = { tls, tls_open, tls_write, tls_read, tls_close };
    return io;
}

#else   // desktop: no ssl service

static int no_tls_open(void *ctx, const char *host, uint16_t port, int timeout_ms, char *err, size_t err_size)
{
    (void)ctx;
    (void)host;
    (void)port;
    (void)timeout_ms;
    snprintf(err, err_size, "TLS is only available on the console");
    return -1;
}

static int no_write(void *ctx, const void *buf, size_t len, int timeout_ms)
{
    (void)ctx;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

static long no_read(void *ctx, void *buf, size_t len, int timeout_ms)
{
    (void)ctx;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

static void no_close(void *ctx)
{
    (void)ctx;
}

BlIo bl_tls_io(BlTls *tls, const char *ca_file)
{
    memset(tls, 0, sizeof(*tls));
    tls->tcp.fd = -1;
    tls->ca_file = ca_file;
    BlIo io = { tls, no_tls_open, no_write, no_read, no_close };
    return io;
}

#endif
