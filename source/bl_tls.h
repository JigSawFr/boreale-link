// bl_tls — the MQTT stream over TLS, through the console's own ssl service
// (its certificate store; a private certificate authority can be added from
// a PEM file). Switch only: on the desktop build opening fails with a
// sentence saying so (the simulator talks to a plain local broker).
// The handshake uses the service's blocking mode, which gives up after
// 5 minutes; reads wait with sslConnectionPoll.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "bl_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BlTcp       tcp;
    const char *ca_file;     // PEM path, NULL or "": the system store only
    bool        ssl_open;    // sslInitialize done
    bool        ctx_open, conn_open;
    // libnx's SslContext / SslConnection (each a Service plus an id), kept
    // opaque here so this header builds without <switch.h>.
    uint64_t    ctx[4], conn[4];
} BlTls;

BlIo bl_tls_io(BlTls *tls, const char *ca_file);

#ifdef __cplusplus
}
#endif
