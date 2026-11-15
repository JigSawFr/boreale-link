// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "agent_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sync_conf.h"

void agent_shared_init(AgentShared *s, const char *version)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->version, sizeof(s->version), "%s", version ? version : "");
    snprintf(s->status.version, sizeof(s->status.version), "%s", s->version);
}

static uint32_t push_doc(char *doc, size_t cap, AgentDocInfo *info, const AgentCall *c)
{
    if (!c->buf || !c->buf_len) return AGENT_RC_BAD_INPUT;
    // A document may arrive with its terminating NUL.
    size_t n = c->buf_len;
    while (n && ((const char *)c->buf)[n - 1] == '\0') n--;
    if (n >= cap) return AGENT_RC_TOO_LARGE;
    memcpy(doc, c->buf, n);
    doc[n] = '\0';
    info->len = n;
    info->fresh = true;
    return 0;
}

static uint32_t push_final(AgentShared *s, const AgentCall *c)
{
    // "YYYY-MM-DD\n" then the document (none: the day is cleared).
    const char *b = (const char *)c->buf;
    if (!b || c->buf_len < 11 || b[10] != '\n') return AGENT_RC_BAD_INPUT;
    for (int i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? b[i] != '-' : (b[i] < '0' || b[i] > '9')) return AGENT_RC_BAD_INPUT;
    size_t n = c->buf_len - 11;
    while (n && b[11 + n - 1] == '\0') n--;
    if (n >= AGENT_DOC_FINAL) return AGENT_RC_TOO_LARGE;
    // The same day again replaces it; else a free slot.
    AgentFinal *slot = NULL;
    for (int i = 0; i < AGENT_FINALS && !slot; i++)
        if (!strncmp(s->finals[i].date, b, 10) && s->finals[i].date[0]) slot = &s->finals[i];
    for (int i = 0; i < AGENT_FINALS && !slot; i++)
        if (!s->finals[i].fresh) slot = &s->finals[i];
    if (!slot) return AGENT_RC_FULL;
    memcpy(slot->date, b, 10);
    slot->date[10] = '\0';
    memcpy(slot->doc, b + 11, n);
    slot->doc[n] = '\0';
    slot->len = n;
    slot->fresh = true;
    return 0;
}

static uint32_t pop_order(AgentShared *s, AgentCall *c)
{
    if (!c->buf_out || c->buf_out_cap < sizeof(AgentOrder)) return AGENT_RC_BAD_INPUT;
    AgentOrder none;
    memset(&none, 0, sizeof(none));
    const AgentOrder *o = &none;
    for (int i = 0; i < AGENT_ORDERS_MAX; i++)
        if (s->fwd[i].id && !s->fwd_popped[i]) {
            o = &s->fwd[i];
            s->fwd_popped[i] = true;
            break;
        }
    memcpy(c->buf_out, o, sizeof(*o));
    c->buf_out_len = sizeof(*o);
    return 0;
}

static uint32_t order_result(AgentShared *s, const AgentCall *c)
{
    if (c->in_len < sizeof(AgentResult)) return AGENT_RC_BAD_INPUT;
    AgentResult r;
    memcpy(&r, c->in, sizeof(r));
    for (int i = 0; i < AGENT_ORDERS_MAX; i++) {
        if (s->fwd[i].id != r.id || !r.id) continue;
        if (s->n_results == AGENT_ORDERS_MAX) return AGENT_RC_FULL;
        s->results[s->n_results++] = r;
        memset(&s->fwd[i], 0, sizeof(s->fwd[i]));
        s->fwd_popped[i] = false;
        return 0;
    }
    return AGENT_RC_BAD_INPUT;   // not an order the agent handed out (any more)
}

static uint32_t get_log(AgentShared *s, AgentCall *c)
{
    size_t at = 0;
    char *out = (char *)c->buf_out;
    if (!out || !c->buf_out_cap) return AGENT_RC_BAD_INPUT;
    const uint32_t first = (s->log_next + AGENT_LOG_LINES - s->log_count) % AGENT_LOG_LINES;
    for (uint32_t k = 0; k < s->log_count; k++) {
        const char *line = s->log[(first + k) % AGENT_LOG_LINES];
        const size_t n = strlen(line);
        if (at + n + 1 >= c->buf_out_cap) break;
        memcpy(out + at, line, n);
        at += n;
        out[at++] = '\n';
    }
    out[at] = '\0';
    c->buf_out_len = at;
    return 0;
}

uint32_t agent_dispatch(AgentShared *s, uint32_t cmd, AgentCall *c)
{
    c->out_len = 0;
    c->buf_out_len = 0;
    // Everything but Hello needs PlayGuard's session first.
    if (cmd != AgentCmd_Hello && cmd != AgentCmd_GetStatus && cmd != AgentCmd_GetLog && !s->app_session)
        return AGENT_RC_NO_SESSION;
    switch (cmd) {
    case AgentCmd_Hello: {
        // A Hello of another size (another protocol) still gets its reply,
        // with accepted 0: only its protocol is read.
        if (c->in_len < sizeof(uint32_t) || c->out_cap < sizeof(AgentHelloReply)) return AGENT_RC_BAD_INPUT;
        AgentHello h;
        memset(&h, 0, sizeof(h));
        memcpy(&h, c->in, c->in_len < sizeof(h) ? c->in_len : sizeof(h));
        h.version[AGENT_VERSION_MAX - 1] = '\0';
        AgentHelloReply r;
        memset(&r, 0, sizeof(r));
        r.protocol = AGENT_PROTOCOL;
        snprintf(r.version, sizeof(r.version), "%s", s->version);
        snprintf(r.console_id, sizeof(r.console_id), "%s", s->console_id);
        r.online = s->status.link.state == SyncLink_Online;
        // A PlayGuard of another protocol gets the reply (it offers to update
        // the agent) but no session: the agent stays on its own.
        r.accepted = h.protocol == AGENT_PROTOCOL && c->in_len >= sizeof(h);
        memcpy(c->out, &r, sizeof(r));
        c->out_len = sizeof(r);
        if (!r.accepted) return 0;
        // A stop asked for (an update) that did not happen: the link again.
        s->want_shutdown = false;
        s->app_protocol = h.protocol;
        snprintf(s->app_version, sizeof(s->app_version), "%s", h.version);
        if (!s->app_session) {
            s->app_session = true;
            s->app_foreground = true;   // it says otherwise when it is not
            s->session_changed = true;
        }
        return 0;
    }
    case AgentCmd_SetForeground: {
        if (c->in_len < 1) return AGENT_RC_BAD_INPUT;
        const bool fg = ((const uint8_t *)c->in)[0] != 0;
        if (fg != s->app_foreground) s->foreground_changed = true;
        s->app_foreground = fg;
        return 0;
    }
    case AgentCmd_PushState: return push_doc(s->state, sizeof(s->state), &s->state_info, c);
    case AgentCmd_PushActivity: return push_doc(s->activity, sizeof(s->activity), &s->activity_info, c);
    case AgentCmd_PushNames: return push_doc(s->names, sizeof(s->names), &s->names_info, c);
    case AgentCmd_PushWeek: return push_doc(s->week, sizeof(s->week), &s->week_info, c);
    case AgentCmd_PushGames: return push_doc(s->games, sizeof(s->games), &s->games_info, c);
    case AgentCmd_PushFinal: return push_final(s, c);
    case AgentCmd_PopOrder: return pop_order(s, c);
    case AgentCmd_OrderResult: return order_result(s, c);
    case AgentCmd_SyncNow:
        s->want_sync_now = true;
        return 0;
    case AgentCmd_GetStatus: {
        if (!c->buf_out || c->buf_out_cap < sizeof(AgentStatus)) return AGENT_RC_BAD_INPUT;
        AgentStatus st = s->status;
        st.app_session = s->app_session;
        st.foreground = s->app_foreground;
        memcpy(c->buf_out, &st, sizeof(st));
        c->buf_out_len = sizeof(st);
        return 0;
    }
    case AgentCmd_GetRecords:
        if (c->out_cap < sizeof(AgentRecords)) return AGENT_RC_BAD_INPUT;
        memcpy(c->out, &s->records, sizeof(s->records));
        c->out_len = sizeof(s->records);
        return 0;
    case AgentCmd_ReloadConfig:
        s->want_reload = true;
        return 0;
    case AgentCmd_GetLog: return get_log(s, c);
    case AgentCmd_PrepareShutdown:
        s->want_shutdown = true;
        return 0;
    default: return AGENT_RC_UNKNOWN_CMD;
    }
}

size_t agent_session_closed(AgentShared *s)
{
    size_t n = 0;
    for (int i = 0; i < AGENT_ORDERS_MAX; i++) {
        if (!s->fwd[i].id) continue;
        if (s->n_closed_ids < AGENT_ORDERS_MAX) s->closed_ids[s->n_closed_ids++] = s->fwd[i].id;
        n++;
        memset(&s->fwd[i], 0, sizeof(s->fwd[i]));
        s->fwd_popped[i] = false;
    }
    s->app_session = false;
    s->app_foreground = false;
    s->session_changed = true;
    // What it pushed is PlayGuard's view: the agent reads the console again.
    s->state_info.fresh = s->activity_info.fresh = false;
    return n;
}

size_t agent_take_closed(AgentShared *s, uint32_t *ids, size_t max)
{
    size_t n = s->n_closed_ids < max ? s->n_closed_ids : max;
    memcpy(ids, s->closed_ids, n * sizeof(*ids));
    memmove(s->closed_ids, s->closed_ids + n, (s->n_closed_ids - n) * sizeof(*ids));
    s->n_closed_ids -= (uint32_t)n;
    return n;
}

bool agent_forward(AgentShared *s, const AgentOrder *o)
{
    for (int i = 0; i < AGENT_ORDERS_MAX; i++)
        if (!s->fwd[i].id) {
            s->fwd[i] = *o;
            s->fwd_popped[i] = false;
            return true;
        }
    return false;
}

size_t agent_take_results(AgentShared *s, AgentResult *out, size_t max)
{
    size_t n = s->n_results < max ? s->n_results : max;
    memcpy(out, s->results, n * sizeof(*out));
    memmove(s->results, s->results + n, (s->n_results - n) * sizeof(*out));
    s->n_results -= (uint32_t)n;
    return n;
}

size_t agent_take_doc(const char *doc, AgentDocInfo *info, char *out, size_t cap)
{
    if (!info->fresh || info->len >= cap) return 0;
    memcpy(out, doc, info->len);
    out[info->len] = '\0';
    info->fresh = false;
    return info->len;
}

void agent_log(AgentShared *s, const char *line)
{
    snprintf(s->log[s->log_next], AGENT_LOG_LINE, "%s", line);
    s->log_next = (s->log_next + 1) % AGENT_LOG_LINES;
    if (s->log_count < AGENT_LOG_LINES) s->log_count++;
}

typedef struct {
    AgentNroState *n;
    bool           saw_read_only;
} NroCtx;

static void nro_kv(void *ctx, const char *key, const char *value)
{
    NroCtx *c = (NroCtx *)ctx;
    AgentNroState *n = c->n;
    bool b = false;
    if (!strcmp(key, "read_only")) {
        if (bl_parse_bool(value, &b)) {
            n->read_only = b;
            c->saw_read_only = true;
        }
    } else if (!strcmp(key, "firmware")) {
        snprintf(n->firmware, sizeof(n->firmware), "%s", value);
    } else if (!strcmp(key, "serial")) {
        // Letters and digits only (a serial number): anything else is not one.
        size_t i = 0;
        for (; value[i] && i < sizeof(n->serial) - 1; i++)
            if (!isalnum((unsigned char)value[i])) break;
        if (value[i] == '\0' && i > 0) snprintf(n->serial, sizeof(n->serial), "%s", value);
    }
}

void agent_nro_parse(AgentNroState *n, const char *text, size_t len)
{
    memset(n, 0, sizeof(*n));
    sync_records_clear(&n->records);
    if (!text) return;
    n->known = true;
    // Unknown until the file says otherwise: a file without the keys (an
    // older PlayGuard) never allows a write.
    n->read_only = true;
    sync_records_parse(&n->records, text, len);
    NroCtx ctx = { n, false };
    bl_kv_each(text, len, nro_kv, &ctx);
    if (!ctx.saw_read_only) n->read_only = true;   // the switch must be in the file to allow anything
}

bool agent_may_write(const AgentNroState *n, const char *firmware_now)
{
    if (!n->known || n->read_only || !n->firmware[0] || !firmware_now) return false;
    return !strcmp(n->firmware, firmware_now);
}
