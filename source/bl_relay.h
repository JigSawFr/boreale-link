// bl_relay — what the relay service `bl:link` is made of, apart from the
// IPC itself. A homebrew that runs while an agent holds the console's one
// broker connection hands its documents to the agent, which publishes them;
// the agent hands back the orders that came for that homebrew. This file is
// the part both sides agree on and the agent must not get wrong: the wire
// constants, which topics a module may publish (only under its own level,
// never another's, never a wildcard), and the bounded queue of orders kept
// for one session. It moves bytes and names topics; it adds no right to the
// agent. Allocation-free, no thread, no console service: it builds for the
// host as for the Switch.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_RELAY_SERVICE  "bl:link"
// The protocol of the relay's commands. A change to a structure the two sides
// exchange, or to a command's meaning, raises it; an agent that speaks another
// one refuses the session and the homebrew falls back to its own connection.
#define BL_RELAY_PROTOCOL 1

#define BL_RELAY_SUB_MAX     96     // a topic below the module's level, with its NUL
#define BL_RELAY_PAYLOAD_MAX 4096   // one document (the session's receive buffer is as large)
#define BL_RELAY_ORDER_MAX   256    // one order's payload: orders are short
#define BL_RELAY_QUEUE       8      // orders kept for a session before the oldest ones wait at the broker

typedef enum {
    BlRelayCmd_Hello    = 0,   // in: protocol, module, version. out: accepted, console id, online
    BlRelayCmd_Publish  = 1,   // in buffer: flags, sub topic, payload
    BlRelayCmd_PopOrder = 2,   // out buffer: the sub topic and payload of one order (empty: none)
    BlRelayCmd_Status   = 3,   // out: online, orders waiting, publishes refused
    BlRelayCmd_Close    = 4,   // the session ends (a crash ends it as well)
    // Control: asked of the agent itself, on a session that said no Hello (an agent
    // from before them answers AGENT_RC_UNKNOWN_CMD: it is too old).
    BlRelayCmd_Info     = 5,   // out: the agent's protocol and version, whether it is online or leaving
    BlRelayCmd_Shutdown = 6,   // leave the broker ("offline" said) and stay away: a manager is about to stop or replace it
    BlRelayCmd_Resume   = 7,   // come back: the update did not happen, or is done (a restart comes back by itself)
} BlRelayCmd;

// The structures the two sides exchange (the same compiler, the same ABI:
// aarch64), checked at build time: a change to any of them raises
// BL_RELAY_PROTOCOL and its size is written here.
#define BL_RELAY_VERSION_MAX 24
#define BL_RELAY_ID_LEN      8      // the console's id: 8 lower-case hex digits
#define BL_RELAY_SIZE_HELLO       64
#define BL_RELAY_SIZE_HELLO_REPLY 16
#define BL_RELAY_SIZE_STATUS      8
#define BL_RELAY_SIZE_INFO        32

// Hello, in. The agent reads `protocol` from a Hello of any size.
typedef struct {
    uint32_t protocol;
    char     module[BL_APP_MODULE_MAX];
    char     version[BL_RELAY_VERSION_MAX];   // the homebrew's
    uint8_t  pad[3];
} BlRelayHello;

// Hello, out. Always answered with success: `accepted` says whether the
// session opened (another protocol, a module that may not relay, one already
// open, or no room: 0, and the homebrew keeps its own connection).
typedef struct {
    uint32_t protocol;
    char     console_id[BL_RELAY_ID_LEN + 1];
    uint8_t  online;
    uint8_t  accepted;
    uint8_t  pad[1];
} BlRelayHelloReply;

typedef struct {
    uint32_t refused;      // orders refused, the queue being full
    uint8_t  online;
    uint8_t  orders;       // waiting for the homebrew
    uint8_t  outbox;       // publications waiting for the agent's loop
    uint8_t  pad;
} BlRelayStatus;

// Info, out: what an app that manages the agent (installs, updates, stops it)
// asks before and after: does it answer, which version, which protocol.
typedef struct {
    uint32_t protocol;                          // BL_RELAY_PROTOCOL of the agent
    char     version[BL_RELAY_VERSION_MAX];     // the agent's own
    uint8_t  online;                            // its broker connection is up
    uint8_t  leaving;                           // Shutdown was asked and not undone
    uint8_t  modules;                           // homebrews with a session open
    uint8_t  pad;
} BlRelayInfo;

#ifdef __cplusplus
#define BL_RELAY_SIZE_CHECK(type, size) static_assert(sizeof(type) == (size), #type ": bump BL_RELAY_PROTOCOL")
#else
#define BL_RELAY_SIZE_CHECK(type, size) _Static_assert(sizeof(type) == (size), #type ": bump BL_RELAY_PROTOCOL")
#endif
BL_RELAY_SIZE_CHECK(BlRelayHello, BL_RELAY_SIZE_HELLO);
BL_RELAY_SIZE_CHECK(BlRelayHelloReply, BL_RELAY_SIZE_HELLO_REPLY);
BL_RELAY_SIZE_CHECK(BlRelayStatus, BL_RELAY_SIZE_STATUS);
BL_RELAY_SIZE_CHECK(BlRelayInfo, BL_RELAY_SIZE_INFO);
#undef BL_RELAY_SIZE_CHECK

// The buffers of Publish (in) and PopOrder (out): a flags byte (bit 0:
// retained), the topic below the module's level, a NUL, then the payload.
// `bl_relay_encode_publish` writes one (0: refused, the topic or the payload
// does not fit); `bl_relay_decode_order` reads one the agent handed back,
// pointing into `buf` (false: malformed, the topic not terminated or not
// allowed).
size_t bl_relay_encode_publish(const char *sub, const void *payload, size_t len, bool retained, void *out,
                               size_t cap);
bool   bl_relay_decode_order(const void *buf, size_t len, const char **sub, const void **payload, size_t *payload_len,
                             bool *retained);

// Whether `module` may open a relay session: a valid registry name that is
// not one of the levels of the console itself (`availability`, `system`, …),
// which the agent alone publishes.
bool bl_relay_module_ok(const char *module);

// Whether `sub` is a topic a module may name below its level: non-empty
// levels of letters, digits, '_', '-' and '.', separated by '/', no level
// "." or "..", no leading or trailing '/', under BL_RELAY_SUB_MAX bytes. No
// wildcard, no `$`, nothing that could climb out of the module's level.
bool bl_relay_sub_ok(const char *sub);

// `<module>/<sub>`: what is appended to the session's base (`<prefix>/<id>`).
// False when the module or the sub is refused or `out` is too small.
bool bl_relay_topic(const char *module, const char *sub, char *out, size_t cap);

// The reverse, for what the broker brings: `rel` is a topic below the base,
// `<module>/<sub>`. Fills `module` (cap BL_APP-sized) and points `*sub` into
// `rel`. False when it is not under a module that may relay.
bool bl_relay_split(const char *rel, char *module, size_t module_cap, const char **sub);

// One order waiting for a session.
typedef struct {
    uint8_t  retained;
    uint16_t len;
    char     sub[BL_RELAY_SUB_MAX];
    char     payload[BL_RELAY_ORDER_MAX];
} BlRelayOrder;

// The orders of one session, oldest first. Full: the new order is refused and
// counted, not the old ones dropped (an order already accepted by the
// broker's acknowledgement is not taken back).
typedef struct {
    BlRelayOrder items[BL_RELAY_QUEUE];
    uint8_t      head, count;
    uint32_t     refused;
} BlRelayQueue;

void bl_relay_queue_init(BlRelayQueue *q);
// False: refused (full, sub or payload too long, sub not allowed).
bool bl_relay_queue_push(BlRelayQueue *q, const char *sub, const void *payload, size_t len, bool retained);
// Moves the oldest into `out`. False: none waiting.
bool bl_relay_queue_pop(BlRelayQueue *q, BlRelayOrder *out);

#ifdef __cplusplus
}
#endif
