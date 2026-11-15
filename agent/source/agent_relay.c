// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "agent_relay.h"

#include <stdio.h>
#include <string.h>

_Static_assert(SYNC_ID_LEN == BL_RELAY_ID_LEN, "the console id the relay says is the one the sync engine uses");
_Static_assert(AGENT_VERSION_MAX == BL_RELAY_VERSION_MAX, "the version a Hello carries");

void agent_relay_init(AgentRelay *r)
{
    memset(r, 0, sizeof(*r));
}

void agent_relay_set_link(AgentRelay *r, const char *console_id, bool online)
{
    snprintf(r->console_id, sizeof(r->console_id), "%s", console_id ? console_id : "");
    r->online = online;
}

static AgentRelaySession *session_at(AgentRelay *r, int slot)
{
    return slot >= 0 && slot < AGENT_RELAY_SESSIONS && r->sessions[slot].open ? &r->sessions[slot] : NULL;
}

static AgentRelaySession *find_module(AgentRelay *r, const char *module)
{
    for (int i = 0; i < AGENT_RELAY_SESSIONS; i++)
        if (r->sessions[i].open && !strcmp(r->sessions[i].module, module)) return &r->sessions[i];
    return NULL;
}

bool agent_relay_is_open(const AgentRelay *r, const char *module)
{
    return module && find_module((AgentRelay *)r, module) != NULL;
}

bool agent_relay_module_at(const AgentRelay *r, int i, char *module, size_t cap, char *version, size_t version_cap)
{
    if (i < 0 || i >= AGENT_RELAY_SESSIONS || !r->sessions[i].open) return false;
    const int n = snprintf(module, cap, "%s", r->sessions[i].module);
    if (n <= 0 || (size_t)n >= cap) return false;
    if (version) snprintf(version, version_cap, "%s", r->sessions[i].version);
    return true;
}

static uint32_t hello(AgentRelay *r, int *slot, AgentCall *c)
{
    if (!c->out || c->out_cap < sizeof(BlRelayHelloReply)) return AGENT_RC_BAD_INPUT;
    // The protocol is read from a Hello of any size (the layout may change).
    uint32_t protocol = 0;
    if (c->in && c->in_len >= sizeof(protocol)) memcpy(&protocol, c->in, sizeof(protocol));
    BlRelayHelloReply reply;
    memset(&reply, 0, sizeof(reply));
    reply.protocol = BL_RELAY_PROTOCOL;
    memcpy(reply.console_id, r->console_id, sizeof(reply.console_id));
    reply.online = r->online ? 1 : 0;

    BlRelayHello in;
    if (*slot < 0 && protocol == BL_RELAY_PROTOCOL && c->in_len >= sizeof(in)) {
        memcpy(&in, c->in, sizeof(in));
        in.module[sizeof(in.module) - 1] = '\0';
        in.version[sizeof(in.version) - 1] = '\0';
        // PlayGuard's own level is the agent's (its app talks pg:agent).
        if (bl_relay_module_ok(in.module) && strcmp(in.module, "playguard") && !find_module(r, in.module)) {
            for (int i = 0; i < AGENT_RELAY_SESSIONS; i++) {
                if (r->sessions[i].open) continue;
                AgentRelaySession *s = &r->sessions[i];
                memset(s, 0, sizeof(*s));
                s->open = true;
                snprintf(s->module, sizeof(s->module), "%s", in.module);
                snprintf(s->version, sizeof(s->version), "%s", in.version);
                bl_relay_queue_init(&s->orders);
                *slot = i;
                reply.accepted = 1;
                break;
            }
        }
    }
    memcpy(c->out, &reply, sizeof(reply));
    c->out_len = sizeof(reply);
    return 0;
}

static uint32_t publish(AgentRelaySession *s, const AgentCall *c)
{
    const char *b = (const char *)c->buf;
    if (!b || c->buf_len < 3) return AGENT_RC_BAD_INPUT;
    const bool retained = (b[0] & 1) != 0;
    const char *sub = b + 1;
    const size_t max_sub = c->buf_len - 1;
    const size_t sub_len = strnlen(sub, max_sub);
    if (sub_len == max_sub || sub_len >= BL_RELAY_SUB_MAX) return AGENT_RC_BAD_INPUT;   // no NUL, or too long
    const char *payload = sub + sub_len + 1;
    size_t n = c->buf_len - 1 - sub_len - 1;
    char topic[BL_APP_MODULE_MAX + BL_RELAY_SUB_MAX];
    if (!bl_relay_topic(s->module, sub, topic, sizeof(topic))) return AGENT_RC_BAD_INPUT;
    if (n > BL_RELAY_PAYLOAD_MAX) return AGENT_RC_TOO_LARGE;

    // The same topic again replaces the one still waiting; else a free place.
    AgentRelayOut *slot = NULL;
    for (int i = 0; i < AGENT_RELAY_OUTBOX && !slot; i++)
        if (s->out[i].used && !strcmp(s->out[i].sub, sub)) slot = &s->out[i];
    for (int i = 0; i < AGENT_RELAY_OUTBOX && !slot; i++)
        if (!s->out[i].used) slot = &s->out[i];
    if (!slot) return AGENT_RC_FULL;
    slot->used = true;
    slot->retained = retained;
    slot->sub_len = (uint16_t)sub_len;
    snprintf(slot->sub, sizeof(slot->sub), "%s", sub);
    memcpy(slot->payload, payload, n);
    slot->len = n;
    return 0;
}

static uint32_t pop_order(AgentRelaySession *s, AgentCall *c)
{
    // The order goes in the mapped output buffer, its length in the reply
    // (0: none waiting): the client cannot read it from the buffer.
    if (!c->buf_out || c->buf_out_cap < 1 + BL_RELAY_SUB_MAX + BL_RELAY_ORDER_MAX || !c->out ||
        c->out_cap < sizeof(uint32_t))
        return AGENT_RC_BAD_INPUT;
    uint32_t n = 0;
    BlRelayOrder o;
    if (bl_relay_queue_pop(&s->orders, &o)) {
        uint8_t *b = (uint8_t *)c->buf_out;
        const size_t sub_len = strlen(o.sub);
        b[0] = o.retained ? 1 : 0;
        memcpy(b + 1, o.sub, sub_len + 1);
        memcpy(b + 1 + sub_len + 1, o.payload, o.len);
        n = (uint32_t)(1 + sub_len + 1 + o.len);
    }
    memcpy(c->out, &n, sizeof(n));
    c->out_len = sizeof(n);
    return 0;
}

static uint32_t status(AgentRelay *r, AgentRelaySession *s, AgentCall *c)
{
    if (!c->out || c->out_cap < sizeof(BlRelayStatus)) return AGENT_RC_BAD_INPUT;
    BlRelayStatus st;
    memset(&st, 0, sizeof(st));
    st.refused = s->orders.refused;
    st.online = r->online ? 1 : 0;
    st.orders = s->orders.count;
    for (int i = 0; i < AGENT_RELAY_OUTBOX; i++) st.outbox += s->out[i].used ? 1 : 0;
    memcpy(c->out, &st, sizeof(st));
    c->out_len = sizeof(st);
    return 0;
}

uint32_t agent_relay_dispatch(AgentRelay *r, int *slot, uint32_t cmd, AgentCall *c)
{
    if (cmd == BlRelayCmd_Hello) return hello(r, slot, c);
    AgentRelaySession *s = session_at(r, *slot);
    if (!s) return AGENT_RC_NO_SESSION;
    switch (cmd) {
    case BlRelayCmd_Publish:  return publish(s, c);
    case BlRelayCmd_PopOrder: return pop_order(s, c);
    case BlRelayCmd_Status:   return status(r, s, c);
    case BlRelayCmd_Close:    agent_relay_closed(r, *slot); *slot = -1; return 0;
    default:                  return AGENT_RC_UNKNOWN_CMD;
    }
}

void agent_relay_closed(AgentRelay *r, int slot)
{
    if (!session_at(r, slot)) return;
    memset(&r->sessions[slot], 0, sizeof(r->sessions[slot]));
}

bool agent_relay_take_publish(AgentRelay *r, char *topic, size_t topic_cap, void *payload, size_t payload_cap,
                              size_t *payload_len, bool *retained)
{
    for (int i = 0; i < AGENT_RELAY_SESSIONS; i++) {
        AgentRelaySession *s = &r->sessions[i];
        if (!s->open) continue;
        for (int j = 0; j < AGENT_RELAY_OUTBOX; j++) {
            AgentRelayOut *o = &s->out[j];
            if (!o->used) continue;
            if (o->len > payload_cap || !bl_relay_topic(s->module, o->sub, topic, topic_cap)) return false;
            memcpy(payload, o->payload, o->len);
            *payload_len = o->len;
            *retained = o->retained;
            o->used = false;
            return true;
        }
    }
    return false;
}

bool agent_relay_deliver(AgentRelay *r, const char *rel_topic, const void *payload, size_t len, bool retained)
{
    char module[BL_APP_MODULE_MAX];
    const char *sub = NULL;
    if (!bl_relay_split(rel_topic, module, sizeof(module), &sub)) return false;
    AgentRelaySession *s = find_module(r, module);
    return s && bl_relay_queue_push(&s->orders, sub, payload, len, retained);
}
