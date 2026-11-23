// bl_http — one HTTP/1.0 GET over a byte stream (BlIo): plain TCP, or TLS
// through the console's ssl service (bl_tls). Just what the agent's
// checks need: the status, the Location of a redirect, and the body handed
// over in pieces as it arrives (nothing is kept, so a page of any size
// fits a sysmodule's heap). HTTP/1.0 so that no server answers in chunks,
// and no Accept-Encoding so that none compresses. One thread.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_HTTP_LOCATION_MAX 192
#define BL_HTTP_HEAD_MAX     16384  // the head of a response read; a longer one fails

typedef struct {
    int    status;                          // 200, 302…; 0 when no answer
    char   location[BL_HTTP_LOCATION_MAX];  // "" when there is none (or too long)
    size_t body_bytes;                      // handed over
    char   error[96];                       // English, "" when the exchange went through
} BlHttpResult;

// Called with each piece of the body, in order. Return false to stop reading.
typedef bool (*BlHttpBody)(void *ctx, const char *data, size_t len);

// Opens `host:port` on `io`, asks `path`, reads the head and then the body
// (only when `on_body` is not NULL: a redirect's answer is in its head) until
// the stream closes or `max_body` bytes were handed over, then closes `io`.
// `timeout_ms` bounds each wait. Returns true when a status line was read.
bool bl_http_get(BlIo io, const char *host, uint16_t port, const char *path, const char *user_agent,
                 int timeout_ms, BlHttpBody on_body, void *ctx, size_t max_body, BlHttpResult *out);

#ifdef __cplusplus
}
#endif
