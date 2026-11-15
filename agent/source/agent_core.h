// agent_core — what the agent's two threads share, and the pg:agent commands
// on it: the IPC thread (agent_ipc.c) parses a request and calls
// agent_dispatch() with the lock held; the main loop (main.c) takes what
// PlayGuard pushed, the orders' results and its requests (Sync now, reload,
// shutdown), and publishes them. No libnx: host-tested (tests/agent).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "agent_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_DOC_STATE   16384
#define AGENT_DOC_LARGE   65536
#define AGENT_FINALS      7
#define AGENT_DOC_FINAL   8192

typedef struct {
    size_t len;
    bool   fresh;    // pushed since the main loop last took it
} AgentDocInfo;

typedef struct {
    char date[11];
    char doc[AGENT_DOC_FINAL];
    size_t len;
    bool   fresh;
} AgentFinal;

typedef struct {
    // Written by the main loop.
    char        version[AGENT_VERSION_MAX];
    char        console_id[SYNC_ID_LEN + 1];
    AgentStatus status;
    AgentRecords records;
    char        log[AGENT_LOG_LINES][AGENT_LOG_LINE];
    uint32_t    log_next, log_count;

    // Written by PlayGuard through the IPC thread.
    bool         app_session, app_foreground;
    uint32_t     app_protocol;
    char         app_version[AGENT_VERSION_MAX];
    char         state[AGENT_DOC_STATE];
    char         activity[AGENT_DOC_STATE];
    char         names[AGENT_DOC_LARGE];
    char         week[AGENT_DOC_LARGE];
    char         games[AGENT_DOC_LARGE];
    AgentDocInfo state_info, activity_info, names_info, week_info, games_info;
    AgentFinal   finals[AGENT_FINALS];

    // Orders handed to PlayGuard, and their results.
    AgentOrder   fwd[AGENT_ORDERS_MAX];
    bool         fwd_popped[AGENT_ORDERS_MAX];
    AgentResult  results[AGENT_ORDERS_MAX];
    uint32_t     n_results;

    // Orders still out when PlayGuard's session closed: answered "waiting".
    uint32_t     closed_ids[AGENT_ORDERS_MAX];
    uint32_t     n_closed_ids;

    // Requests to the main loop.
    bool want_sync_now, want_reload, want_shutdown;
    bool session_changed;    // opened or closed since the main loop looked
    bool foreground_changed;
} AgentShared;

void agent_shared_init(AgentShared *s, const char *version);

// A pg:agent request: `in` / `in_len` the raw arguments after the CMIF
// header, `buf` the input buffer (documents), `out` the raw reply and
// `buf_out` the output buffer. Returns 0 or an AGENT_RC_*.
typedef struct {
    const void *in;
    size_t      in_len;
    const void *buf;
    size_t      buf_len;
    void       *out;
    size_t      out_cap, out_len;
    void       *buf_out;
    size_t      buf_out_cap, buf_out_len;
} AgentCall;

uint32_t agent_dispatch(AgentShared *s, uint32_t cmd, AgentCall *c);

// PlayGuard's session closed (exit or crash; the IPC thread): the agent is on
// its own again. The orders handed to it and not answered are kept in
// closed_ids, for the main loop to answer "waiting" (they stay retained on
// the broker for the agent to take again). Returns how many.
size_t agent_session_closed(AgentShared *s);
size_t agent_take_closed(AgentShared *s, uint32_t *ids, size_t max);

// The main loop's side.
bool   agent_forward(AgentShared *s, const AgentOrder *o);   // false: no room
size_t agent_take_results(AgentShared *s, AgentResult *out, size_t max);
// Copies a pushed document out when fresh (and marks it taken). 0: nothing new.
size_t agent_take_doc(const char *doc, AgentDocInfo *info, char *out, size_t cap);
void   agent_log(AgentShared *s, const char *line);

// What PlayGuard last wrote in sync/nro_state.txt that the agent needs: its
// records, and whether it may change the console. Read-only when PlayGuard
// was read-only, or when the firmware is not the one PlayGuard last ran on
// (a system update since: nothing PlayGuard has checked).
typedef struct {
    SyncRecords records;
    bool        read_only;
    bool        known;           // the file was there
    char        firmware[16];
    // The serial number PlayGuard read (Atmosphère's PRODINFO backup when
    // blanked), only while publish_serial is on; "" otherwise.
    char        serial[32];
} AgentNroState;

void agent_nro_parse(AgentNroState *n, const char *text, size_t len);
bool agent_may_write(const AgentNroState *n, const char *firmware_now);

#ifdef __cplusplus
}
#endif
