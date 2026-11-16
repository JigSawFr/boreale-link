// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_engine.h"

#include <stdio.h>
#include <string.h>

#include "sync_discovery.h"
#include "sync_entities.h"
#include "sync_state.h"

#define ACTIVITY_PERIOD_MS   60000ULL
#define STATE_SOON_MS        300ULL
// Home Assistant's birth and last-will topic (its MQTT integration's default).
#define HA_STATUS_TOPIC      "homeassistant/status"
// Under <base>: the Home Assistant integration's version (retained, from it).
#define INTEGRATION_TOPIC    "integration"

static void logf_(SyncEngine *e, const char *fmt, const char *a, const char *b)
{
    if (!e->host.log) return;
    snprintf(e->line, sizeof(e->line), fmt, a ? a : "", b ? b : "");
    e->host.log(e->host.ctx, e->line);
}

static uint64_t now_posix(SyncEngine *e)
{
    return e->host.now_posix ? e->host.now_posix(e->host.ctx) : 0;
}

// The session's side of the status, copied into the engine's after each
// call that may change it.
static void mirror(SyncEngine *e)
{
    const BlStatus *b = &e->session.status;
    e->status.state = (SyncLinkState)b->state;
    e->status.protocol = b->protocol;
    memcpy(e->status.error, b->error, sizeof(e->status.error));
    e->status.online_since_ms = b->online_since_ms;
    e->status.last_publish_ms = b->last_publish_ms;
    e->status.next_attempt_ms = b->next_attempt_ms;
    e->status.connects = b->connects;
    e->status.publishes = b->publishes;
}

static bool read_only(SyncEngine *e)
{
    return e->host.read_only && e->host.read_only(e->host.ctx);
}

static void on_message(void *user, const MqttPublish *msg);
static int subscribe(SyncEngine *e);

static void identity(SyncEngine *e)
{
    char id[SYNC_ID_LEN + 1];
    sync_conf_device_id(&e->conf, id);   // this system's: a sysMMC and an emuMMC are two devices
    snprintf(e->base, sizeof(e->base), "%s/%s", e->conf.topic_prefix, id);
    // The agent is the console's one connection, shared by every homebrew: its id carries no module's name.
    if (e->source && !strcmp(e->source, "agent"))
        snprintf(e->client_id, sizeof(e->client_id), "bl-%s-link", id);
    else
        snprintf(e->client_id, sizeof(e->client_id), "pg-%s-%s", id, e->source);
}

static BlSessionConf session_conf(const SyncEngine *e)
{
    BlSessionConf c;
    memset(&c, 0, sizeof(c));
    c.host = e->conf.host;
    c.port = e->conf.port;
    c.username = e->conf.username;
    c.password = e->conf.password;
    c.version = e->conf.mqtt_version == SyncMqtt_V5     ? MQTT_V5
                : e->conf.mqtt_version == SyncMqtt_V311 ? MQTT_V311
                                                        : 0;
    c.base = e->base;
    c.client_id = e->client_id;
    return c;
}

static int session_subscribe(void *ctx)
{
    return subscribe((SyncEngine *)ctx);
}

// Orders not yet handed to the host go with the connection: still retained,
// they come back at the next one.
static void session_lost(void *ctx)
{
    SyncEngine *e = (SyncEngine *)ctx;
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
        if (e->queue[i].id && !e->queue[i].in_flight) memset(&e->queue[i], 0, sizeof(e->queue[i]));
    mirror(e);
}

static uint64_t session_now_posix(void *ctx)
{
    return now_posix((SyncEngine *)ctx);
}

static void session_log(void *ctx, const char *line)
{
    SyncEngine *e = (SyncEngine *)ctx;
    if (e->host.log) e->host.log(e->host.ctx, line);
}

void sync_engine_init(SyncEngine *e, const SyncConf *conf, BlIo io, const SyncHost *host, uint64_t (*now_ms)(void),
                      const char *source, const char *sw_version)
{
    memset(e, 0, sizeof(*e));
    e->conf = *conf;
    e->host = *host;
    e->now_ms = now_ms;
    e->source = source;
    e->sw_version = sw_version;
    e->next_id = 1;
    identity(e);
    const BlSessionConf sc = session_conf(e);
    const BlSessionHost sh = { e, session_subscribe, session_lost, session_now_posix, session_log };
    bl_session_init(&e->session, &sc, io, &sh, now_ms, on_message, e);
}

bool sync_engine_online(const SyncEngine *e)
{
    return bl_session_online(&e->session);
}

const SyncStatus *sync_engine_status(const SyncEngine *e)
{
    return &e->status;
}

size_t sync_engine_pending(const SyncEngine *e)
{
    size_t n = 0;
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) n += e->queue[i].id != 0;
    return n;
}

void sync_engine_sync_now(SyncEngine *e)
{
    e->want_state = e->want_activity = e->want_discovery = e->want_names = e->want_clear = true;
    e->want_capabilities = true;
    e->notices_hash = 0;
    // "online" again: another client of the same console (PlayGuard's own
    // session, handing over to the agent) may have left "offline" behind.
    e->want_online = true;
}

void sync_engine_state_changed(SyncEngine *e)
{
    e->want_state = true;
}

void sync_engine_activity_changed(SyncEngine *e)
{
    e->want_activity = true;
}

void sync_engine_discovery_changed(SyncEngine *e)
{
    e->want_discovery = true;
}

int sync_engine_publish(SyncEngine *e, const char *sub, const char *payload, size_t len, bool retained)
{
    char rel[96];
    if (!sync_topic_sub(sub, rel, sizeof(rel))) return -1;
    const int rc = bl_session_publish(&e->session, rel, payload, len, retained);
    mirror(e);
    return rc;
}

int sync_engine_publish_relay(SyncEngine *e, const char *rel, const void *payload, size_t len, bool retained)
{
    // Only another module's level: not PlayGuard's, not the console's own.
    if (!rel || !strncmp(rel, SYNC_MODULE "/", sizeof(SYNC_MODULE)) || sync_topic_is_device(rel)) return -1;
    const int rc = bl_session_publish(&e->session, rel, payload, len, retained);
    mirror(e);
    return rc;
}

int sync_engine_publish_stream(SyncEngine *e, const char *sub, size_t len, bool retained, SyncReadFn read, void *ctx)
{
    char rel[96];
    if (!sync_topic_sub(sub, rel, sizeof(rel))) return -1;
    const int rc = bl_session_publish_stream(&e->session, rel, len, retained, read, ctx, e->scratch, sizeof(e->scratch));
    mirror(e);
    return rc;
}

void sync_engine_stop(SyncEngine *e)
{
    bl_session_stop(&e->session);
    e->discovery_published = false;
    mirror(e);
}

void sync_engine_reconfigure(SyncEngine *e, const SyncConf *conf)
{
    const bool reconnect = strcmp(conf->host, e->conf.host) || conf->port != e->conf.port || conf->tls != e->conf.tls ||
                           strcmp(conf->ca_file, e->conf.ca_file) || strcmp(conf->username, e->conf.username) ||
                           strcmp(conf->password, e->conf.password) || strcmp(conf->console_id, e->conf.console_id) ||
                           conf->emummc != e->conf.emummc || strcmp(conf->topic_prefix, e->conf.topic_prefix) ||
                           strcmp(conf->discovery_prefix, e->conf.discovery_prefix) ||
                           conf->allow_anonymous != e->conf.allow_anonymous || conf->enabled != e->conf.enabled ||
                           conf->mqtt_version != e->conf.mqtt_version;
    const bool discovery_off = e->conf.ha_discovery && !conf->ha_discovery;
    bl_session_woke(&e->session);
    if (reconnect || discovery_off) {
        // Clear the old discovery while still connected under the old identity.
        if (bl_session_online(&e->session) && e->conf.ha_discovery && (discovery_off || !conf->enabled ||
                                                          strcmp(conf->console_id, e->conf.console_id) ||
                                                          conf->emummc != e->conf.emummc ||
                                                          strcmp(conf->discovery_prefix, e->conf.discovery_prefix))) {
            char topic[160];
            if (sync_discovery_topic(&e->conf, topic, sizeof(topic)))
                bl_session_publish_topic(&e->session, topic, "", 0, true);
            e->discovery_published = false;
        }
    }
    if (reconnect) sync_engine_stop(e);
    e->conf = *conf;
    identity(e);
    const BlSessionConf sc = session_conf(e);
    bl_session_configure(&e->session, &sc, reconnect);   // another broker, or another version asked: "auto" tries 5.0 again
    bl_session_retry_now(&e->session);
    mirror(e);
    sync_engine_sync_now(e);
}

// ---- receiving ----

static uint32_t hash(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *p = a; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ '/') * 16777619u;
    for (const char *p = b; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}

static bool entity_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static void on_message(void *user, const MqttPublish *msg)
{
    SyncEngine *e = (SyncEngine *)user;

    // Home Assistant (re)started: publish everything again. Its birth message
    // is on homeassistant/status whatever the discovery prefix.
    const size_t sn = sizeof(HA_STATUS_TOPIC) - 1;
    if (msg->topic_len == sn && !memcmp(msg->topic, HA_STATUS_TOPIC, sn)) {
        if (msg->payload_len == 6 && !memcmp(msg->payload, "online", 6)) sync_engine_sync_now(e);
        return;
    }

    // <base>/integration: the Home Assistant integration says which version runs.
    const size_t bl = strlen(e->base);
    const size_t il = sizeof(INTEGRATION_TOPIC) - 1;
    if (msg->topic_len == bl + 1 + il && !memcmp(msg->topic, e->base, bl) && msg->topic[bl] == '/' &&
        !memcmp(msg->topic + bl + 1, INTEGRATION_TOPIC, il)) {
        if (msg->payload_len <= SYNC_INTEGRATION_MAX && e->host.integration)
            e->host.integration(e->host.ctx, (const char *)msg->payload, msg->payload_len);
        return;
    }

    // <base>/<module>/…/set for another homebrew, relayed through the agent.
    if (e->host.relay && msg->topic_len > bl + 1 && !memcmp(msg->topic, e->base, bl) && msg->topic[bl] == '/') {
        const char *rel = msg->topic + bl + 1;
        const size_t rl = msg->topic_len - bl - 1;
        const bool own = rl >= sizeof(SYNC_MODULE) && !memcmp(rel, SYNC_MODULE "/", sizeof(SYNC_MODULE));
        if (!own && rl > 4 && !memcmp(rel + rl - 4, "/set", 4)) {
            char topic[96];
            if (rl >= sizeof(topic)) return;
            memcpy(topic, rel, rl);
            topic[rl] = '\0';
            e->host.relay(e->host.ctx, topic, msg->payload, msg->payload_len, msg->retain);
            return;
        }
    }

    // <base>/playguard/<entity>/set: not taken while the module is off (the
    // broker keeps a retained order for when it is on again).
    if (e->no_module) return;
    const size_t ml = sizeof(SYNC_MODULE "/") - 1;
    if (msg->topic_len < bl + 1 + ml + 5 || memcmp(msg->topic, e->base, bl) || msg->topic[bl] != '/' ||
        memcmp(msg->topic + bl + 1, SYNC_MODULE "/", ml))
        return;
    if (memcmp(msg->topic + msg->topic_len - 4, "/set", 4)) return;
    const size_t en = msg->topic_len - bl - 1 - ml - 4;
    if (en == 0 || en >= SYNC_ENTITY_MAX) return;
    char entity[SYNC_ENTITY_MAX];
    memcpy(entity, msg->topic + bl + 1 + ml, en);
    entity[en] = '\0';
    for (size_t i = 0; i < en; i++)
        if (!entity_char(entity[i])) return;

    // An empty payload clears a retained order (ours, echoed back, or the
    // sender taking it back): drop it if still queued.
    if (msg->payload_len == 0) {
        for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
            if (e->queue[i].id && !e->queue[i].in_flight && !strcmp(e->queue[i].entity, entity))
                memset(&e->queue[i], 0, sizeof(e->queue[i]));
        return;
    }
    if (msg->payload_len >= SYNC_PAYLOAD_MAX) {
        e->status.dropped++;
        return;
    }

    // An order stays retained until it is done: a new subscription (a
    // replay, a new connection) hands it over again, RETAIN 1, while it is
    // carried out. Already taken. A press while it runs comes live, RETAIN 0.
    if (msg->retain)
        for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) {
            const SyncQueued *q = &e->queue[i];
            if (q->id && q->in_flight && !strcmp(q->entity, entity) && strlen(q->payload) == msg->payload_len &&
                !memcmp(q->payload, msg->payload, msg->payload_len))
                return;
        }

    // The newest order for an entity replaces one still waiting in the queue.
    SyncQueued *slot = NULL;
    for (size_t i = 0; i < SYNC_QUEUE_MAX && !slot; i++)
        if (e->queue[i].id && !e->queue[i].in_flight && !strcmp(e->queue[i].entity, entity)) slot = &e->queue[i];
    for (size_t i = 0; i < SYNC_QUEUE_MAX && !slot; i++)
        if (!e->queue[i].id) slot = &e->queue[i];
    if (!slot) {
        // Full: it stays retained on the broker and comes back at the next connection.
        e->status.dropped++;
        return;
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->entity, entity, en + 1);
    memcpy(slot->payload, msg->payload, msg->payload_len);
    slot->payload[msg->payload_len] = '\0';
    slot->retained = msg->retain;
    slot->id = e->next_id++;
    if (e->next_id == 0) e->next_id = 1;
    slot->group = slot->id;
    slot->received_ms = e->now_ms();
}

// ---- orders ----

static const char *event_type(const SyncOutcome *out)
{
    if (out->applied) return "command_applied";
    if (out->reason == SyncReason_Waiting) return "command_waiting";
    return "command_rejected";
}

static void publish_event(SyncEngine *e, const char *type, const char *entity, const char *payload, SyncReason reason,
                          bool rc_set, uint32_t rc)
{
    char json[512];
    const size_t n = sync_event_build(e->source, now_posix(e), type, entity, payload, reason, rc_set, rc, json,
                                      sizeof(json));
    if (n) sync_engine_publish(e, "event", json, n, false);
}

void sync_engine_event(SyncEngine *e, const char *type, const char *entity)
{
    publish_event(e, type, entity, NULL, SyncReason_None, false, 0);
}

// Reports one queued order's outcome and frees its slot.
void sync_engine_notices(SyncEngine *e, const SyncNotice *n, size_t count)
{
    for (size_t i = 0; i < count && e->n_notices < SYNC_NOTICE_MAX; i++) e->notices[e->n_notices++] = n[i];
}

static void publish_notices(SyncEngine *e)
{
    for (size_t i = 0; i < e->n_notices; i++) {
        char json[256];
        const size_t n = sync_notice_build(e->source, now_posix(e), &e->notices[i], json, sizeof(json));
        if (n) sync_engine_publish(e, "event", json, n, false);
    }
    e->n_notices = 0;
}

static void finish_one(SyncEngine *e, SyncQueued *q, const SyncOutcome *out)
{
    const bool waiting = out->reason == SyncReason_Waiting;
    const uint32_t h = hash(q->entity, q->payload);
    bool already = false;
    if (waiting) {
        for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) already = already || e->waiting[i] == h;
        if (!already)
            for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
                if (!e->waiting[i]) {
                    e->waiting[i] = h;
                    break;
                }
    } else {
        for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
            if (e->waiting[i] == h) e->waiting[i] = 0;
    }
    if (!already) {
        const bool rc_set = out->rc != 0;
        publish_event(e, event_type(out), q->entity, q->payload, out->reason, rc_set, out->rc);
    }
    if (!waiting) {
        // Used once: clear it on the broker.
        char sub[SYNC_ENTITY_MAX + 8];
        snprintf(sub, sizeof(sub), "%s/set", q->entity);
        sync_engine_publish(e, sub, "", 0, true);
        e->status.orders++;
        if (!out->applied) e->status.rejected++;
        snprintf(e->status.last_result, sizeof(e->status.last_result), "%s: %s", q->entity,
                 out->applied ? "applied" : sync_reason_name(out->reason));
    }
    memset(q, 0, sizeof(*q));
    e->want_state = true;
    e->next_state_ms = e->now_ms() + STATE_SOON_MS;
}

static void finish_group(SyncEngine *e, uint32_t group, const SyncOutcome *out)
{
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
        if (e->queue[i].id && e->queue[i].group == group) finish_one(e, &e->queue[i], out);
}

void sync_engine_order_done(SyncEngine *e, uint32_t id, const SyncOutcome *out)
{
    finish_group(e, id, out);
}

static void reject(SyncEngine *e, SyncQueued *q, SyncReason reason)
{
    SyncOutcome out;
    sync_outcome_init(&out);
    out.reason = reason;
    finish_one(e, q, &out);
}

static void accept(SyncEngine *e, SyncQueued *q)
{
    SyncOutcome out;
    sync_outcome_init(&out);
    out.applied = true;
    finish_one(e, q, &out);
}

static bool is_limit(SyncIntentKind k)
{
    return k == SyncIntent_LimitDay || k == SyncIntent_LimitUniform || k == SyncIntent_LimitsWeek;
}

// Oldest first among the free orders.
static SyncQueued *next_order(SyncEngine *e)
{
    SyncQueued *best = NULL;
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) {
        SyncQueued *q = &e->queue[i];
        if (q->id && !q->in_flight && (!best || q->id < best->id)) best = q;
    }
    return best;
}

static void handle_engine_order(SyncEngine *e, SyncQueued *q, const SyncIntent *in)
{
    if (in->kind == SyncIntent_Discovery) {
        if (e->conf.ha_discovery != in->on) {
            if (!in->on && e->discovery_published) {
                char topic[160];
                if (sync_discovery_topic(&e->conf, topic, sizeof(topic)))
                    bl_session_publish_topic(&e->session, topic, "", 0, true);
                e->discovery_published = false;
            }
            e->conf.ha_discovery = in->on;
            if (e->host.conf_changed) e->host.conf_changed(e->host.ctx, &e->conf);
            if (in->on) e->want_discovery = true;
        }
        accept(e, q);
        return;
    }
    if (in->kind == SyncIntent_SyncNow) {
        sync_engine_sync_now(e);
        accept(e, q);
        return;
    }
    // ExportReport (a host without SyncHost.report gets it as an order)
    const size_t n = e->host.report(e->host.ctx, e->scratch, sizeof(e->scratch));
    if (!n) {
        reject(e, q, SyncReason_Busy);
        return;
    }
    if (e->conf.publish_report) sync_engine_publish(e, "report", e->scratch, n, true);
    accept(e, q);
}

static void process_orders(SyncEngine *e)
{
    const uint64_t now = e->now_ms();
    // Limits keep coming while a slider moves: wait for a pause.
    uint64_t newest_limit = 0;
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) {
        SyncIntent tmp;
        const SyncQueued *q = &e->queue[i];
        if (q->id && !q->in_flight && sync_apply_parse(q->entity, q->payload, strlen(q->payload), &tmp) == SyncReason_None &&
            is_limit(tmp.kind) && q->received_ms > newest_limit)
            newest_limit = q->received_ms;
    }
    const bool limits_settled = newest_limit == 0 || now - newest_limit >= SYNC_LIMIT_SETTLE_MS;

    for (int guard = 0; guard < SYNC_QUEUE_MAX; guard++) {
        SyncQueued *q = next_order(e);
        if (!q || !bl_session_online(&e->session)) return;
        SyncIntent in;
        const SyncReason parsed = sync_apply_parse(q->entity, q->payload, strlen(q->payload), &in);
        if (parsed != SyncReason_None) {
            reject(e, q, parsed);
            continue;
        }
        // A report the host cannot write here (it reads the console on
        // another thread) goes to it like an order.
        const bool host_report = in.kind == SyncIntent_ExportReport && !e->host.report;
        if (!sync_intent_on_console(in.kind) && !host_report) {
            if (e->conf.policy == SyncPolicy_Off && !sync_intent_harmless(in.kind) && in.kind != SyncIntent_Discovery) {
                reject(e, q, SyncReason_PolicyOff);
                continue;
            }
            handle_engine_order(e, q, &in);
            continue;
        }
        if (!host_report) {
            if (e->conf.policy == SyncPolicy_Off) {
                reject(e, q, SyncReason_PolicyOff);
                continue;
            }
            if (sync_intent_needs_timer_writes(in.kind) && !e->conf.remote_timer_writes) {
                reject(e, q, SyncReason_TimerWritesDisabled);
                continue;
            }
            if (read_only(e)) {
                reject(e, q, SyncReason_ReadOnly);
                continue;
            }
        }
        if (is_limit(in.kind)) {
            if (!limits_settled) return;   // the next turn
            // Every limit order waiting, in the order they came, as one week.
            SyncIntent week;
            memset(&week, 0, sizeof(week));
            week.kind = SyncIntent_LimitsWeek;
            for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) {
                SyncQueued *c = &e->queue[i];
                SyncIntent ci;
                if (!c->id || c->in_flight) continue;
                if (sync_apply_parse(c->entity, c->payload, strlen(c->payload), &ci) != SyncReason_None ||
                    !is_limit(ci.kind))
                    continue;
                c->group = q->id;
            }
            // Apply them oldest first, so a later one wins on the same day.
            uint32_t last = 0;
            for (;;) {
                SyncQueued *m = NULL;
                for (size_t i = 0; i < SYNC_QUEUE_MAX; i++) {
                    SyncQueued *c = &e->queue[i];
                    if (c->id && c->group == q->id && c->id > last && (!m || c->id < m->id)) m = c;
                }
                if (!m) break;
                last = m->id;
                SyncIntent ci;
                sync_apply_parse(m->entity, m->payload, strlen(m->payload), &ci);
                for (int d = 0; d < 7; d++)
                    if (ci.mask & (1u << d)) week.days[d] = ci.days[d];
                week.mask |= ci.mask;
            }
            in = week;
        }
        SyncOutcome out;
        sync_outcome_init(&out);
        const bool final = e->host.order(e->host.ctx, q->group, &in, q->entity, q->payload, q->retained, &out);
        if (final) {
            finish_group(e, q->group, &out);
        } else {
            const uint32_t g = q->group;
            for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
                if (e->queue[i].id && e->queue[i].group == g) e->queue[i].in_flight = true;
        }
    }
}

// ---- publishing ----

static void publish_discovery(SyncEngine *e)
{
    char topic[160];
    if (!sync_discovery_topic(&e->conf, topic, sizeof(topic))) return;
    const size_t n_profiles = e->host.profiles ? e->host.profiles(e->host.ctx, e->profiles, 16) : 0;
    if (e->host.device) e->host.device(e->host.ctx, &e->device);
    SyncDiscovery d = { &e->conf, e->sw_version, read_only(e), (const char(*)[SYNC_PROFILE_MAX])e->profiles, n_profiles,
                        &e->device };
    const size_t n = sync_discovery_build(&d, e->scratch, sizeof(e->scratch));
    if (!n) {
        logf_(e, "remote link: the discovery does not fit%s%s", NULL, NULL);
        return;
    }
    if (bl_session_publish_topic(&e->session, topic, e->scratch, n, true) == 0) {
        e->session.status.publishes++;
        e->discovery_published = true;
    }
}

// What is not published leaves nothing on the broker: its documents are
// cleared (an empty retained publish) at each connection and when the
// settings change. The finished days (activity/<date>) are PlayGuard's to
// clear, which knows their dates.
static void clear_unpublished(SyncEngine *e)
{
    if (e->no_module) {
        // Without PlayGuard's module nothing of its stays on the broker: its
        // documents and the console's discovery as the agent published them.
        // The finished days (activity/<date>) are PlayGuard's to clear, which
        // knows their dates.
        static const char *const OWN[] = { "state", "activity", "week", "names", "games", "report" };
        for (size_t i = 0; i < sizeof(OWN) / sizeof(OWN[0]); i++) sync_engine_publish(e, OWN[i], "", 0, true);
        char topic[160];
        if (sync_discovery_topic(&e->conf, topic, sizeof(topic))) bl_session_publish_topic(&e->session, topic, "", 0, true);
        e->discovery_published = false;
        return;
    }
    static const struct {
        SyncPublish what;
        const char *sub;
    } DOCS[] = {
        { SyncPublish_Activity, "activity" },
        { SyncPublish_Activity, "week" },
        { SyncPublish_Activity, "names" },
        { SyncPublish_Games, "games" },
        { SyncPublish_Report, "report" },
    };
    for (size_t i = 0; i < sizeof(DOCS) / sizeof(DOCS[0]); i++)
        if (!sync_conf_publishes(&e->conf, DOCS[i].what)) sync_engine_publish(e, DOCS[i].sub, "", 0, true);
}

// "YYYY-MM-DD" of the UTC day `days` after 1970-01-01 (H. Hinnant's
// civil_from_days).
static void date_of(int64_t days, char out[32])
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    snprintf(out, 32, "%04d-%02u-%02u", (int)y, m, d);
}

static void clear_topic(SyncEngine *e, const char *topic)
{
    bl_session_publish_topic(&e->session, topic, "", 0, true);
}

// A schema-1 settings file published under <legacy>/<id>/ with no module
// level, and the discovery's device was playguard_<id>: what is left of
// that tree is cleared once (empty retained publishes), then the settings
// saved without it. The icons are left to PlayGuard, which knows which it
// sent (icons.txt).
static void clear_legacy(SyncEngine *e)
{
    char id[SYNC_ID_LEN + 1], old[96], topic[160];
    sync_conf_device_id(&e->conf, id);
    snprintf(old, sizeof(old), "%s/%s", e->conf.legacy_prefix, id);
    static const char *const DOCS[] = { "state", "activity", "week", "names", "games", "report" };
    for (size_t i = 0; i < sizeof(DOCS) / sizeof(DOCS[0]); i++) {
        snprintf(topic, sizeof(topic), "%s/%s", old, DOCS[i]);
        clear_topic(e, topic);
    }
    // At the same prefix, the availability and the system document are
    // where they were; under another one, they go too.
    if (strcmp(e->conf.legacy_prefix, e->conf.topic_prefix)) {
        snprintf(topic, sizeof(topic), "%s/availability", old);
        clear_topic(e, topic);
        snprintf(topic, sizeof(topic), "%s/system", old);
        clear_topic(e, topic);
    }
    // Orders still retained on the old topics would never be read again.
    size_t n = 0;
    const SyncEntity *list = sync_entities(&n);
    for (size_t i = 0; i < n; i++) {
        if (list[i].write == SyncWrite_None) continue;
        snprintf(topic, sizeof(topic), "%s/%s/set", old, list[i].id);
        clear_topic(e, topic);
    }
    // The finished days (the console keeps 14), a day either side for the
    // time zone.
    const uint64_t t = now_posix(e);
    for (int d = 0; t && d < 16; d++) {
        char date[32];
        date_of((int64_t)(t / 86400) + 1 - d, date);
        snprintf(topic, sizeof(topic), "%s/activity/%s", old, date);
        clear_topic(e, topic);
    }
    snprintf(topic, sizeof(topic), "%s/device/playguard_%s/config", e->conf.discovery_prefix, id);
    clear_topic(e, topic);
    if (!bl_session_online(&e->session)) return;   // cut short: tried again at the next connection
    logf_(e, "remote link: cleared the topics under %s/ (before %s)", old, e->conf.topic_prefix);
    e->conf.legacy_prefix[0] = '\0';
    if (e->host.conf_changed) e->host.conf_changed(e->host.ctx, &e->conf);
}

// Every homebrew's shared bell (the host's sync_notices), when it changed.
static void publish_shared_bell(SyncEngine *e)
{
    const size_t n = e->host.notices && bl_session_online(&e->session)
                         ? e->host.notices(e->host.ctx, e->scratch, sizeof(e->scratch)) : 0;
    if (!n) return;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)e->scratch[i]) * 16777619u;
    h = h ? h : 1;
    if (h != e->notices_hash && sync_engine_publish(e, "notices", e->scratch, n, true) == 0) e->notices_hash = h;
}

static void publish_due(SyncEngine *e)
{
    const uint64_t now = e->now_ms();
    if (e->want_discovery) {
        e->want_discovery = false;
        if (e->conf.ha_discovery && !e->no_module) publish_discovery(e);
    }
    if (!bl_session_online(&e->session)) return;
    if (e->want_clear) {
        e->want_clear = false;
        clear_unpublished(e);
    }
    if (e->want_online && bl_session_announce(&e->session) >= 0) e->want_online = false;
    if (e->want_capabilities) {
        SyncRelayModule relayed[SYNC_RELAY_MODULES];
        size_t n_relayed = 0;
        for (size_t i = 0; i < e->n_relay_modules; i++) {
            if (!e->relay_open[i]) continue;
            snprintf(relayed[n_relayed].name, sizeof(relayed[n_relayed].name), "%s", e->relay_modules[i]);
            snprintf(relayed[n_relayed].version, sizeof(relayed[n_relayed].version), "%s", e->relay_versions[i]);
            n_relayed++;
        }
        const size_t n = sync_capabilities_build(e->sw_version, !e->no_module, relayed, n_relayed, e->scratch,
                                                 sizeof(e->scratch));
        if (n && sync_engine_publish(e, "capabilities", e->scratch, n, true) == 0) e->want_capabilities = false;
    }
    if (e->conf.legacy_prefix[0]) clear_legacy(e);
    if (e->want_state || now >= e->next_state_ms) {
        if (now >= e->next_state_ms || e->want_state) {
            size_t n = e->host.state && !e->no_module ? e->host.state(e->host.ctx, e->scratch, sizeof(e->scratch)) : 0;
            if (n) sync_engine_publish(e, "state", e->scratch, n, true);
            if (!e->no_module) publish_notices(e);   // what building the state found (the host's sync_watch)
            n = e->host.system && bl_session_online(&e->session) ? e->host.system(e->host.ctx, e->scratch, sizeof(e->scratch)) : 0;
            if (n) sync_engine_publish(e, "system", e->scratch, n, true);
            publish_shared_bell(e);
            if (n && e->host.device && e->conf.ha_discovery && !e->no_module) {
                SyncDevice dev;
                e->host.device(e->host.ctx, &dev);
                if (dev.model != e->device.model || dev.hos_version != e->device.hos_version ||
                    dev.ams_version != e->device.ams_version || strcmp(dev.nickname, e->device.nickname))
                    e->want_discovery = true;
            }
            e->want_state = false;
            e->next_state_ms = now + (uint64_t)e->conf.poll_s * 1000ULL;
        }
    }
    if (!bl_session_online(&e->session)) return;
    if (!e->no_module && e->conf.publish_activity && e->host.activity && (e->want_activity || now >= e->next_activity_ms)) {
        const size_t n = e->host.activity(e->host.ctx, e->scratch, sizeof(e->scratch));
        if (n) sync_engine_publish(e, "activity", e->scratch, n, true);
        e->want_activity = false;
        e->next_activity_ms = now + ACTIVITY_PERIOD_MS;
    }
}

static void relay_filter(const SyncEngine *e, size_t i, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/+/set", e->base, e->relay_modules[i]);
}

static int subscribe(SyncEngine *e)
{
    char orders[160], integration[160];
    char relay[SYNC_RELAY_MODULES][160];
    snprintf(orders, sizeof(orders), "%s/" SYNC_MODULE "/+/set", e->base);
    snprintf(integration, sizeof(integration), "%s/" INTEGRATION_TOPIC, e->base);
    const char *topics[3 + SYNC_RELAY_MODULES] = { orders, HA_STATUS_TOPIC, integration };
    uint8_t qos[3 + SYNC_RELAY_MODULES] = { 1, 0, 1 };
    size_t count = 3;
    if (e->no_module) {   // PlayGuard's orders are not ours to take: the other two filters only
        topics[0] = HA_STATUS_TOPIC;
        topics[1] = integration;
        qos[0] = 0;
        qos[1] = 1;
        count = 2;
    }
    // One filter per module relayed, never `+/+/set`: that one would cover
    // PlayGuard's own orders a second time.
    for (size_t i = 0; i < e->n_relay_modules; i++) {
        relay_filter(e, i, relay[i], sizeof(relay[i]));
        topics[count] = relay[i];
        qos[count++] = 1;
    }
    // The integration's version, and the other homebrews' orders, are
    // optional: a broker's access list that leaves them out does not keep the
    // link down.
    const size_t base_count = e->no_module ? 2 : 3;
    const int refused = bl_session_subscribe(&e->session, topics, qos, count, e->no_module ? 1 : 2);
    if (refused > 0 && count == base_count)
        logf_(e, "remote link: the broker refused %s%s, the integration's version stays unknown", integration, NULL);
    else if (refused > 0)
        logf_(e, "remote link: the broker refused %s or an order topic of another homebrew", integration, NULL);
    return refused < 0 ? -1 : 0;
}

// The module's place in the engine's list, adding (and subscribing to) it when
// it is new; -1: PlayGuard's own, an unusable name, or no room.
static int relay_slot(SyncEngine *e, const char *module)
{
    if (!module || !*module || strlen(module) >= SYNC_RELAY_MODULE_MAX || !strcmp(module, SYNC_MODULE)) return -1;
    for (size_t i = 0; i < e->n_relay_modules; i++)
        if (!strcmp(e->relay_modules[i], module)) return (int)i;
    if (e->n_relay_modules >= SYNC_RELAY_MODULES) return -1;
    const size_t at = e->n_relay_modules++;
    snprintf(e->relay_modules[at], SYNC_RELAY_MODULE_MAX, "%s", module);
    // Connected: this module's orders now (retained ones come back with it);
    // else the next connection subscribes to them with the rest.
    if (bl_session_online(&e->session)) {
        char filter[160];
        relay_filter(e, at, filter, sizeof(filter));
        const char *topics[1] = { filter };
        const uint8_t qos[1] = { 1 };
        if (bl_session_subscribe(&e->session, topics, qos, 1, 0) > 0)
            logf_(e, "remote link: the broker refused %s", filter, NULL);
    }
    return (int)at;
}

void sync_engine_relay_sync(SyncEngine *e, const SyncRelayModule *open, size_t n_open)
{
    bool was_open[SYNC_RELAY_MODULES];
    char was_version[SYNC_RELAY_MODULES][24];
    for (size_t i = 0; i < SYNC_RELAY_MODULES; i++) {
        was_open[i] = e->relay_open[i];
        memcpy(was_version[i], e->relay_versions[i], sizeof(was_version[i]));
        e->relay_open[i] = false;
    }
    for (size_t i = 0; open && i < n_open; i++) {
        const int at = relay_slot(e, open[i].name);
        if (at < 0) continue;
        e->relay_open[at] = true;
        snprintf(e->relay_versions[at], sizeof(e->relay_versions[at]), "%s", open[i].version);
    }
    for (size_t i = 0; i < SYNC_RELAY_MODULES; i++) {
        if (e->relay_open[i] != was_open[i] || (e->relay_open[i] && strcmp(was_version[i], e->relay_versions[i])))
            e->want_capabilities = true;
    }
}

void sync_engine_set_module(SyncEngine *e, bool on)
{
    if (e->no_module == !on) return;
    e->no_module = !on;
    e->want_capabilities = true;
    if (!on) e->want_clear = true;
    if (on) {
        // Everything of PlayGuard's again, and its orders back from the broker.
        e->want_state = e->want_activity = e->want_discovery = e->want_names = true;
        if (bl_session_online(&e->session) && subscribe(e) < 0) bl_session_lost(&e->session);
    }
}

void sync_engine_replay_orders(SyncEngine *e)
{
    bl_session_woke(&e->session);
    if (bl_session_online(&e->session) && subscribe(e) < 0) bl_session_lost(&e->session);
    mirror(e);
}

int sync_engine_step(SyncEngine *e, int max_wait_ms)
{
    bl_session_woke(&e->session);
    if (!e->conf.enabled || sync_conf_problem(&e->conf)) {
        if (e->session.mqtt.open) sync_engine_stop(e);
        e->session.status.state = BlLink_Off;
        snprintf(e->session.status.error, sizeof(e->session.status.error), "%s",
                 e->conf.enabled ? sync_conf_problem(&e->conf) : "");
        mirror(e);
        return max_wait_ms;
    }

    bool connected = false;
    const int sleep = bl_session_connect_due(&e->session, max_wait_ms, &connected);
    mirror(e);
    if (sleep >= 0) return sleep;
    if (connected) {
        memset(e->waiting, 0, sizeof(e->waiting));
        sync_engine_sync_now(e);
        e->next_state_ms = 0;
        e->next_activity_ms = 0;
        logf_(e, "remote link: online (%s, MQTT %s)", e->conf.host,
              e->session.mqtt.version == MQTT_V5 ? "5.0" : "3.1.1");
    }

    process_orders(e);
    if (bl_session_online(&e->session)) publish_due(e);
    if (!bl_session_check(&e->session)) {
        mirror(e);
        return 0;
    }

    // Wait for the broker, but not past the next thing due.
    uint64_t wait = (uint64_t)(max_wait_ms > 0 ? max_wait_ms : 0);
    const uint64_t t = e->now_ms();
    if (e->next_state_ms > t && e->next_state_ms - t < wait) wait = e->next_state_ms - t;
    if (e->want_state || e->want_discovery || e->want_activity) wait = 0;
    for (size_t i = 0; i < SYNC_QUEUE_MAX; i++)
        if (e->queue[i].id && !e->queue[i].in_flight) wait = wait > 100 ? 100 : wait;
    bl_session_poll(&e->session, (int)wait);
    mirror(e);
    return 0;
}
