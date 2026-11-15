// agent_ipc — the pg:agent service between PlayGuard and the agent sysmodule
// (docs/sync-protocol.md, "IPC service"). The agent is the only MQTT client
// while it runs; PlayGuard, while open, pushes what it reads of the console
// and carries out the orders the agent hands it. Command ids and the fixed
// structures below are the wire format of both sides (same compiler, same
// ABI: aarch64). The IPC message itself holds 256 bytes: JSON documents,
// logs, orders and the status travel in mapped buffers.
//
// A session open means PlayGuard runs; in the foreground it is the only
// process that reads pctl (the agent publishes what it pushes); in the
// background, or once the session closes (exit or crash), the agent reads the
// console again by itself.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync_apply.h"
#include "sync_engine.h"
#include "sync_exec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_SERVICE_NAME "pg:agent"
// 2: the 1.0.0 layout (sd:/config/playguard, each system's files in
// nand/<system>/sync/). 3: PushGames. An agent of another protocol is
// refused, and PlayGuard offers the one it carries.
#define AGENT_PROTOCOL     3
// The size of each structure exchanged (and of the ones they embed), checked
// at build time below: a change to any of them fails the build until it is
// written here and AGENT_PROTOCOL goes up. AgentHello and AgentHelloReply
// never change: they are how two protocols find out they differ.
#define AGENT_SIZE_HELLO        28
#define AGENT_SIZE_HELLO_REPLY  44
#define AGENT_SIZE_ORDER        212
#define AGENT_SIZE_RESULT       12
#define AGENT_SIZE_STATUS       304
#define AGENT_SIZE_RECORDS      42
#define AGENT_SIZE_SYNC_INTENT  68
#define AGENT_SIZE_SYNC_STATUS  264
#define AGENT_SIZE_SYNC_RECORDS 34
#define AGENT_VERSION_MAX  24
#define AGENT_ORDERS_MAX   SYNC_QUEUE_MAX
#define AGENT_LOG_LINES    200
#define AGENT_LOG_LINE     120

// The agent's results ("module" 412 in the Result encoding).
#define AGENT_RC(desc)        ((uint32_t)(412u | ((uint32_t)(desc) << 9)))
#define AGENT_RC_UNKNOWN_CMD  AGENT_RC(1)
#define AGENT_RC_BAD_INPUT    AGENT_RC(2)
#define AGENT_RC_TOO_LARGE    AGENT_RC(3)
#define AGENT_RC_PROTOCOL     AGENT_RC(4)
#define AGENT_RC_NO_SESSION   AGENT_RC(5)
#define AGENT_RC_FULL         AGENT_RC(6)   // no room: try again later

typedef enum {
    AgentCmd_Hello           = 0,   // in AgentHello, out AgentHelloReply
    AgentCmd_SetForeground   = 1,   // in u8
    AgentCmd_PushState       = 2,   // in buffer: the state document
    AgentCmd_PushActivity    = 3,   // in buffer: today's activity
    AgentCmd_PushNames       = 4,   // in buffer: the names document
    AgentCmd_PushWeek        = 5,   // in buffer: the week document
    AgentCmd_PushFinal       = 6,   // in buffer: "YYYY-MM-DD\n" then a finished day's activity
    AgentCmd_PopOrder        = 7,   // out buffer: AgentOrder (id 0: none waiting)
    AgentCmd_OrderResult     = 8,   // in AgentResult
    AgentCmd_SyncNow         = 9,
    AgentCmd_GetStatus       = 10,  // out buffer: AgentStatus
    AgentCmd_GetRecords      = 12,  // out AgentRecords
    AgentCmd_ReloadConfig    = 13,  // sync.conf and nro_state.txt changed
    AgentCmd_GetLog          = 14,  // out buffer: the last lines, oldest first, one per line
    AgentCmd_PrepareShutdown = 15,  // "offline", disconnect: an update or a stop follows
    AgentCmd_PushGames       = 16,  // in buffer: the games document
} AgentCmd;

// Frozen across protocols, as is its reply: any PlayGuard must be able to
// say hello to any agent and learn its protocol. The agent reads `protocol`
// from a Hello of any size.
typedef struct {
    uint32_t protocol;
    char     version[AGENT_VERSION_MAX];   // PlayGuard's
} AgentHello;

// Always answered with success (a reply that comes with an error is not
// delivered): `accepted` says whether PlayGuard got its session (0: another
// protocol, PlayGuard offers to update the agent). Frozen, as AgentHello.
typedef struct {
    uint32_t protocol;
    char     version[AGENT_VERSION_MAX];   // the agent's
    char     console_id[SYNC_ID_LEN + 1];
    uint8_t  online;
    uint8_t  accepted;
    uint8_t  pad[5];
} AgentHelloReply;

// An order the agent hands PlayGuard (the engine already parsed it, and
// merged several limit orders into one: the intent is what to carry out).
typedef struct {
    uint32_t   id;          // 0: no order waiting
    uint8_t    retained;
    uint8_t    pad[3];
    char       entity[SYNC_ENTITY_MAX];
    char       payload[SYNC_PAYLOAD_MAX];
    SyncIntent intent;
} AgentOrder;

typedef struct {
    uint32_t id;
    uint32_t rc;
    uint8_t  applied, changed, reason, relock_failed;
} AgentResult;

typedef struct {
    SyncStatus link;
    uint32_t   pending;        // orders not answered yet
    uint8_t    app_session;    // PlayGuard holds a session
    uint8_t    foreground;     // and is in the foreground
    uint8_t    configured;     // sync.conf turns the link on and is complete
    uint8_t    read_only;      // the agent refuses writes (see agent_core.h)
    char       version[AGENT_VERSION_MAX];
    uint64_t   uptime_ms;
} AgentStatus;

typedef struct {
    uint8_t     valid;
    uint8_t     pad[7];
    SyncRecords records;       // what the agent changed (extra time, console lock, relock)
} AgentRecords;

#ifdef __cplusplus
#define AGENT_SIZE_CHECK(type, size) static_assert(sizeof(type) == (size), #type ": bump AGENT_PROTOCOL")
#else
#define AGENT_SIZE_CHECK(type, size) _Static_assert(sizeof(type) == (size), #type ": bump AGENT_PROTOCOL")
#endif
AGENT_SIZE_CHECK(AgentHello, AGENT_SIZE_HELLO);
AGENT_SIZE_CHECK(AgentHelloReply, AGENT_SIZE_HELLO_REPLY);
AGENT_SIZE_CHECK(AgentOrder, AGENT_SIZE_ORDER);
AGENT_SIZE_CHECK(AgentResult, AGENT_SIZE_RESULT);
AGENT_SIZE_CHECK(AgentStatus, AGENT_SIZE_STATUS);
AGENT_SIZE_CHECK(AgentRecords, AGENT_SIZE_RECORDS);
AGENT_SIZE_CHECK(SyncIntent, AGENT_SIZE_SYNC_INTENT);
AGENT_SIZE_CHECK(SyncStatus, AGENT_SIZE_SYNC_STATUS);
AGENT_SIZE_CHECK(SyncRecords, AGENT_SIZE_SYNC_RECORDS);
#undef AGENT_SIZE_CHECK

#ifdef __cplusplus
}
#endif
