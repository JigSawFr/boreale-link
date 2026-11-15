// bl_relay_nx — the client of the agent's `bl:link` service (bl_relay.h): a
// homebrew that is open asks the agent that holds the console's broker
// connection to publish its documents and to hand back its orders, instead of
// opening a second connection. One session per process. Switch only: elsewhere
// `bl_relay_open` says the agent is not there, and the homebrew keeps its own
// connection.
//
// Not thread-safe: one thread drives it, as the session of bl_session.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "bl_relay.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether an agent serves `bl:link` (the service is registered; sm is not
// asked for one that is not, it would wait until it is).
bool bl_relay_available(void);

// Opens the session for `module` (a BL_APP-valid name) and says Hello.
// True when the agent accepted it: `reply->accepted` is then 1, and the
// homebrew publishes through it. False: no agent, another protocol, a module
// it refuses or no room (the reply, when given, says which); the homebrew
// keeps its own connection. `result` (may be NULL) gets the console's result
// code of the failing call, 0 otherwise.
bool bl_relay_open(const char *module, const char *version, BlRelayHelloReply *reply, uint32_t *result);
bool bl_relay_connected(void);
void bl_relay_close(void);

// 0 or the console's result code; a call that fails because the agent went
// away ends the session (`bl_relay_connected` turns false).
uint32_t bl_relay_publish(const char *sub, const void *payload, size_t len, bool retained);
// The next order for this module: `*got` false when none waits. The pointers
// of `out` stay valid until the next call.
typedef struct {
    uint8_t     buf[1 + BL_RELAY_SUB_MAX + BL_RELAY_ORDER_MAX];
    const char *sub;
    const void *payload;
    size_t      payload_len;
    bool        retained;
} BlRelayInOrder;
uint32_t bl_relay_pop_order(BlRelayInOrder *out, bool *got);
uint32_t bl_relay_status(BlRelayStatus *out);

#ifdef __cplusplus
}
#endif
