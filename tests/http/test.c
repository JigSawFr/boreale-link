// bl_http: a GET over a scripted stream: the head read a line at a time (a
// head of several KB, split anywhere), a redirect's Location, a body handed
// over in pieces, and each way the exchange fails.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_http.h"
#include "check.h"

typedef struct {
    const char *answer;     // what the "server" says
    size_t      answer_len;
    size_t      piece;      // bytes handed per read
    size_t      at;
    bool        refuse;     // open fails
    bool        silent;     // reads time out
    char        sent[512];
    bool        closed;
} Fake;

static int f_open(void *ctx, const char *host, uint16_t port, int t, char *err, size_t n)
{
    Fake *f = ctx;
    (void)host; (void)port; (void)t;
    if (f->refuse) {
        snprintf(err, n, "refused");
        return -1;
    }
    return 0;
}
static int f_write(void *ctx, const void *buf, size_t len, int t)
{
    Fake *f = ctx;
    (void)t;
    snprintf(f->sent, sizeof(f->sent), "%.*s", (int)len, (const char *)buf);
    return 0;
}
static long f_read(void *ctx, void *buf, size_t len, int t)
{
    Fake *f = ctx;
    (void)t;
    if (f->silent) return 0;
    if (f->at >= f->answer_len) return -1;
    size_t n = f->answer_len - f->at;
    if (n > f->piece) n = f->piece;
    if (n > len) n = len;
    memcpy(buf, f->answer + f->at, n);
    f->at += n;
    return (long)n;
}
static void f_close(void *ctx) { ((Fake *)ctx)->closed = true; }

static BlIo io_of(Fake *f)
{
    BlIo io = { f, f_open, f_write, f_read, f_close };
    return io;
}

typedef struct { char text[1024]; size_t len; size_t calls; size_t stop_after; } Body;

static bool on_body(void *ctx, const char *data, size_t len)
{
    Body *b = ctx;
    if (b->len + len < sizeof(b->text)) memcpy(b->text + b->len, data, len);
    b->len += len;
    b->calls++;
    return !b->stop_after || b->len < b->stop_after;
}

static void test_redirect(void)
{
    // GitHub's head: many headers, the Location in the middle, over 2 KB.
    char big[4096];
    int n = snprintf(big, sizeof(big), "HTTP/1.1 302 Found\r\nServer: GitHub.com\r\nContent-Security-Policy: ");
    for (int i = 0; i < 200; i++) n += snprintf(big + n, sizeof(big) - (size_t)n, "x-%d ", i);
    n += snprintf(big + n, sizeof(big) - (size_t)n,
                  "\r\nlocation: https://github.com/CTCaer/hekate/releases/tag/v6.5.4\r\nContent-Length: 0\r\n\r\n");
    for (size_t piece = 1; piece <= 600; piece += (piece < 10 ? 1 : 197)) {
        Fake f = { big, (size_t)n, piece, 0, false, false, "", false };
        BlHttpResult r;
        CHECK(bl_http_get(io_of(&f), "github.com", 443, "/CTCaer/hekate/releases/latest", "boreale-link/1", 5000, NULL,
                          NULL, 0, &r));
        CHECK(r.status == 302 && !strcmp(r.location, "https://github.com/CTCaer/hekate/releases/tag/v6.5.4"));
        CHECK(f.closed && strstr(f.sent, "GET /CTCaer/hekate/releases/latest HTTP/1.0\r\nHost: github.com\r\n"));
        CHECK(strstr(f.sent, "User-Agent: boreale-link/1\r\n") && !strstr(f.sent, "Accept-Encoding"));
    }
}

static void test_body(void)
{
    const char *a = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nline one\n| [[23.0.1]]\n";
    for (size_t piece = 1; piece <= 80; piece += 13) {
        Fake f = { a, strlen(a), piece, 0, false, false, "", false };
        Body b = { "", 0, 0, 0 };
        BlHttpResult r;
        CHECK(bl_http_get(io_of(&f), "x.org", 443, "/", NULL, 5000, on_body, &b, 0, &r));
        CHECK(r.status == 200 && r.body_bytes == strlen("line one\n| [[23.0.1]]\n") && b.len == r.body_bytes);
        CHECK(!strncmp(b.text, "line one\n| [[23.0.1]]\n", b.len));
    }
    // A bound on the body, and a caller that stops.
    Fake f = { a, strlen(a), 10, 0, false, false, "", false };
    Body b = { "", 0, 0, 0 };
    BlHttpResult r;
    CHECK(bl_http_get(io_of(&f), "x.org", 443, "/", NULL, 5000, on_body, &b, 5, &r) && r.body_bytes == 5 && b.len == 5);
    Fake g = { a, strlen(a), 10, 0, false, false, "", false };
    Body c = { "", 0, 0, 4 };
    CHECK(bl_http_get(io_of(&g), "x.org", 443, "/", NULL, 5000, on_body, &c, 0, &r) && c.len >= 4 && g.closed);
}

static void test_errors(void)
{
    BlHttpResult r;
    Fake refuse = { "", 0, 10, 0, true, false, "", false };
    CHECK(!bl_http_get(io_of(&refuse), "x.org", 443, "/", NULL, 100, NULL, NULL, 0, &r) && strstr(r.error, "refused") && r.status == 0);

    Fake silent = { "", 0, 10, 0, false, true, "", false };
    CHECK(!bl_http_get(io_of(&silent), "x.org", 443, "/", NULL, 100, NULL, NULL, 0, &r) && r.error[0] && silent.closed);

    const char *junk = "<html>not http at all\r\n\r\n";
    Fake j = { junk, strlen(junk), 8, 0, false, false, "", false };
    CHECK(!bl_http_get(io_of(&j), "x.org", 443, "/", NULL, 100, NULL, NULL, 0, &r) && r.status == 0 && r.error[0]);

    const char *cut = "HTTP/1.1 200 OK\r\nServer: x\r\n";   // closed before the blank line
    Fake c = { cut, strlen(cut), 8, 0, false, false, "", false };
    CHECK(!bl_http_get(io_of(&c), "x.org", 443, "/", NULL, 100, NULL, NULL, 0, &r) && r.error[0]);

    const char *nf = "HTTP/1.1 404 Not Found\r\n\r\nbody";
    Fake n = { nf, strlen(nf), 8, 0, false, false, "", false };
    Body b = { "", 0, 0, 0 };
    CHECK(bl_http_get(io_of(&n), "x.org", 443, "/", NULL, 100, on_body, &b, 0, &r) && r.status == 404 && b.len == 0);

    // A head that never ends is cut off.
    char *endless = malloc(40000);
    memset(endless, 'a', 40000);
    memcpy(endless, "HTTP/1.1 200 OK\r\n", 17);
    Fake e = { endless, 40000, 500, 0, false, false, "", false };
    CHECK(!bl_http_get(io_of(&e), "x.org", 443, "/", NULL, 100, NULL, NULL, 0, &r) && r.error[0]);
    free(endless);

    // A path too long for the request.
    char path[600];
    memset(path, 'p', sizeof(path));
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    Fake l = { "", 0, 10, 0, false, false, "", false };
    CHECK(!bl_http_get(io_of(&l), "x.org", 443, path, NULL, 100, NULL, NULL, 0, &r) && l.closed);
}

int main(void)
{
    test_redirect();
    test_body();
    test_errors();
    return CHECK_DONE("http: head, redirect, body and failures assertions passed");
}
