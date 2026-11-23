// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(BlHttpResult *out, const char *what, const char *detail)
{
    snprintf(out->error, sizeof(out->error), "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static bool starts_with_nocase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    return true;
}

// The head, read a line at a time as it arrives: GitHub's carries headers
// of several KB, and only the status and the Location matter.
#define HEAD_TOTAL_MAX BL_HTTP_HEAD_MAX

typedef struct {
    char   line[BL_HTTP_LOCATION_MAX + 16];
    size_t len, total;
    bool   over, first;
} Head;

// One whole line (without its line break). Returns 1 at the blank line that ends the head, -1 when it is not HTTP.
static int head_line(Head *h, BlHttpResult *out)
{
    if (h->first) {
        h->first = false;
        if (h->over || strncmp(h->line, "HTTP/1.", 7) != 0) return -1;
        const char *sp = strchr(h->line, ' ');
        out->status = sp ? atoi(sp + 1) : 0;
        if (out->status < 100 || out->status > 599) {
            out->status = 0;
            return -1;
        }
        return 0;
    }
    if (h->len == 0 && !h->over) return 1;
    if (!h->over && starts_with_nocase(h->line, "location:")) {
        const char *v = h->line + 9;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(out->location, sizeof(out->location), "%s", v);
    }
    return 0;
}

// Feeds `len` bytes of the head; returns how many it took: all of them, or up to
// and with the blank line (then *state is 1), or *state -1.
static size_t head_feed(Head *h, const char *data, size_t len, BlHttpResult *out, int *state)
{
    *state = 0;
    for (size_t i = 0; i < len; i++) {
        if (++h->total > HEAD_TOTAL_MAX) {
            *state = -1;
            return i + 1;
        }
        if (data[i] == '\n') {
            if (h->len && h->line[h->len - 1] == '\r') h->len--;
            h->line[h->len] = '\0';
            const int r = head_line(h, out);
            h->len = 0;
            h->over = false;
            if (r) {
                *state = r;
                return i + 1;
            }
        } else if (h->len + 1 < sizeof(h->line)) {
            h->line[h->len++] = data[i];
        } else {
            h->over = true;
        }
    }
    return len;
}

bool bl_http_get(BlIo io, const char *host, uint16_t port, const char *path, const char *user_agent,
                 int timeout_ms, BlHttpBody on_body, void *ctx, size_t max_body, BlHttpResult *out)
{
    memset(out, 0, sizeof(*out));
    char err[96] = "";
    if (io.open(io.ctx, host, port, timeout_ms, err, sizeof(err)) != 0) {
        fail(out, "cannot connect", err);
        return false;
    }
    char request[384];
    const int n = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                           path, host, user_agent ? user_agent : "boreale-link");
    if (n < 0 || (size_t)n >= sizeof(request)) {
        io.close(io.ctx);
        fail(out, "request too long", NULL);
        return false;
    }
    if (io.write(io.ctx, request, (size_t)n, timeout_ms) != 0) {
        io.close(io.ctx);
        fail(out, "cannot send the request", NULL);
        return false;
    }

    Head head = { .first = true };
    bool got_head = false, stopped = false;
    for (;;) {
        char chunk[512];
        const long r = io.read(io.ctx, chunk, sizeof(chunk), timeout_ms);
        if (r < 0) break;                          // closed: the end of the body
        if (r == 0) {
            fail(out, "no answer", NULL);
            break;
        }
        const char *data = chunk;
        size_t len = (size_t)r;
        if (!got_head) {
            int state;
            const size_t took = head_feed(&head, data, len, out, &state);
            if (state < 0) {
                fail(out, head.total > HEAD_TOTAL_MAX ? "the answer's head is too long" : "not an HTTP answer", NULL);
                break;
            }
            if (state == 0) continue;
            got_head = true;
            data += took;
            len -= took;
            if (!on_body || out->status != 200) break;   // a redirect or an error: the head says it
        }
        if (len && on_body) {
            if (max_body && out->body_bytes + len > max_body) len = max_body - out->body_bytes;
            if (len && !on_body(ctx, data, len)) stopped = true;
            out->body_bytes += len;
            if (stopped || (max_body && out->body_bytes >= max_body)) break;
        }
    }
    io.close(io.ctx);
    if (!got_head && !out->error[0]) fail(out, "no answer", NULL);
    return got_head;
}
