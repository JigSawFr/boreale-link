// agent_ipc — the pg:agent service (source/sync/agent_ipc.h): a thread blocked
// in svcReplyAndReceive on the service's port and PlayGuard's sessions. Each
// request is parsed (HIPC message, CMIF header), handed to agent_dispatch()
// under the shared lock, and answered at the next svcReplyAndReceive. Mapped
// buffers (documents in; the log, an order, the status out) are read and
// written in place. Closing the session PlayGuard said Hello on — by Close
// or because PlayGuard ended — gives the agent its independence back.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>
#include <switch.h>

#include "agent_core.h"
#include "agent_relay.h"

extern AgentShared g_shared;
extern AgentRelay  g_relay;
extern Mutex       g_lock;

#define MAX_SESSIONS 6
#define MAX_HANDLES  (2 + MAX_SESSIONS)
#define IN_MAX       0x100

// What each handle is: the two ports, then the sessions on them.
typedef enum { K_PORT_PG, K_PORT_RELAY, K_PG, K_RELAY } Kind;

static Thread s_thread;
static Handle s_handles[MAX_HANDLES];
static Kind   s_kind[MAX_HANDLES];
static int    s_slot[MAX_HANDLES];           // a relay session's place in g_relay (-1: not open)
static s32    s_count;
static Handle s_app;                         // the session that said Hello

static void close_session(s32 index)
{
    const Handle h = s_handles[index];
    if (s_kind[index] == K_RELAY) {
        mutexLock(&g_lock);
        agent_relay_closed(&g_relay, s_slot[index]);
        mutexUnlock(&g_lock);
    } else if (h == s_app) {
        s_app = INVALID_HANDLE;
        mutexLock(&g_lock);
        if (g_shared.app_session) agent_session_closed(&g_shared);
        mutexUnlock(&g_lock);
    }
    svcCloseHandle(h);
    for (s32 i = index; i + 1 < s_count; i++) {
        s_handles[i] = s_handles[i + 1];
        s_kind[i] = s_kind[i + 1];
        s_slot[i] = s_slot[i + 1];
    }
    s_count--;
}

static void reply(u8 *base, uint32_t rc, const void *out, size_t out_len)
{
    const u32 words = (u32)((sizeof(CmifOutHeader) + out_len + 0x10 + 3) / 4);
    HipcRequest r = hipcMakeRequestInline(base, .type = CmifCommandType_Request, .num_data_words = words);
    CmifOutHeader *h = (CmifOutHeader *)cmifGetAlignedDataStart(r.data_words, base);
    h->magic = CMIF_OUT_HEADER_MAGIC;
    h->version = 0;
    h->result = rc;
    h->token = 0;
    if (out_len) memcpy(h + 1, out, out_len);
}

// The request on s_handles[index]; false when the session is to be closed.
static bool handle(s32 index)
{
    u8 *base = (u8 *)armGetTls();
    const HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.type == CmifCommandType_Close) return false;
    if (r.meta.type != CmifCommandType_Request) {
        reply(base, AGENT_RC_UNKNOWN_CMD, NULL, 0);
        return true;
    }
    const CmifInHeader *hdr = (const CmifInHeader *)cmifGetAlignedDataStart(r.data.data_words, base);
    const size_t words = (size_t)r.meta.num_data_words * 4;
    if (words < sizeof(CmifInHeader) + 0x10 || hdr->magic != CMIF_IN_HEADER_MAGIC) {
        reply(base, AGENT_RC_BAD_INPUT, NULL, 0);
        return true;
    }
    // The message is rewritten by the reply: copy the arguments first.
    u8 in[IN_MAX];
    size_t in_len = words - sizeof(CmifInHeader) - 0x10;
    if (in_len > sizeof(in)) in_len = sizeof(in);
    memcpy(in, hdr + 1, in_len);
    const u32 cmd = hdr->command_id;

    AgentCall c;
    memset(&c, 0, sizeof(c));
    c.in = in;
    c.in_len = in_len;
    if (r.meta.num_send_buffers) {
        c.buf = hipcGetBufferAddress(&r.data.send_buffers[0]);
        c.buf_len = hipcGetBufferSize(&r.data.send_buffers[0]);
    }
    if (r.meta.num_recv_buffers) {
        c.buf_out = hipcGetBufferAddress(&r.data.recv_buffers[0]);
        c.buf_out_cap = hipcGetBufferSize(&r.data.recv_buffers[0]);
    }
    u8 out[sizeof(AgentRecords) > sizeof(AgentHelloReply) ? sizeof(AgentRecords) : sizeof(AgentHelloReply)];
    c.out = out;
    c.out_cap = sizeof(out);

    uint32_t rc;
    if (s_kind[index] == K_RELAY) {
        mutexLock(&g_lock);
        rc = agent_relay_dispatch(&g_relay, &s_slot[index], cmd, &c);
        mutexUnlock(&g_lock);
    } else {
        mutexLock(&g_lock);
        const bool had_session = g_shared.app_session;
        rc = agent_dispatch(&g_shared, cmd, &c);
        const bool has_session = g_shared.app_session;
        mutexUnlock(&g_lock);
        if (cmd == AgentCmd_Hello && !had_session && has_session) s_app = s_handles[index];
    }

    reply(base, rc, out, c.out_len);
    return true;
}

static void serve(void *arg)
{
    (void)arg;
    Handle port;
    Result rc = smRegisterService(&port, smEncodeName(AGENT_SERVICE_NAME), false, MAX_SESSIONS);
    if (R_FAILED(rc)) {
        mutexLock(&g_lock);
        agent_log(&g_shared, "pg:agent could not be registered: PlayGuard cannot reach the agent");
        mutexUnlock(&g_lock);
        return;
    }
    s_handles[0] = port;
    s_kind[0] = K_PORT_PG;
    s_slot[0] = -1;
    s_count = 1;
    // The relay is optional: without it the agent serves PlayGuard as before.
    Handle relay_port;
    rc = smRegisterService(&relay_port, smEncodeName(BL_RELAY_SERVICE), false, MAX_SESSIONS);
    if (R_SUCCEEDED(rc)) {
        s_handles[1] = relay_port;
        s_kind[1] = K_PORT_RELAY;
        s_slot[1] = -1;
        s_count = 2;
    } else {
        mutexLock(&g_lock);
        agent_log(&g_shared, "bl:link could not be registered: other homebrews keep their own connection");
        mutexUnlock(&g_lock);
    }
    Handle target = INVALID_HANDLE;
    for (;;) {
        s32 index = -1;
        rc = svcReplyAndReceive(&index, s_handles, s_count, target, UINT64_MAX);
        target = INVALID_HANDLE;
        if (R_FAILED(rc)) {
            // A client gone (ended without Close), or the reply could not
            // be delivered: that session is over.
            if (index >= 0 && index < s_count && s_kind[index] != K_PORT_PG && s_kind[index] != K_PORT_RELAY)
                close_session(index);
            else svcSleepThread(100000000ULL);
            continue;
        }
        if (s_kind[index] == K_PORT_PG || s_kind[index] == K_PORT_RELAY) {
            Handle session;
            if (R_FAILED(svcAcceptSession(&session, s_handles[index]))) continue;
            if (s_count < MAX_HANDLES) {
                s_handles[s_count] = session;
                s_kind[s_count] = s_kind[index] == K_PORT_PG ? K_PG : K_RELAY;
                s_slot[s_count] = -1;
                s_count++;
            } else {
                svcCloseHandle(session);
            }
            continue;
        }
        if (handle(index)) target = s_handles[index];
        else close_session(index);
    }
}

void agent_ipc_start(void)
{
    if (R_FAILED(threadCreate(&s_thread, serve, NULL, NULL, 0x4000, 0x2C, -2))) return;
    threadStart(&s_thread);
}
