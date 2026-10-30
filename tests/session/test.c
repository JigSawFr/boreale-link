// Host tests for bl_session (and mqtt_client under it) against a fake MQTT
// 5.0 / 3.1.1 broker kept in memory (retained messages, subscriptions, last
// will, keep-alive), with a scripted clock: the connection, its last will
// and "online", the subscriptions, publishing, the pause between attempts,
// the version fallback, the keep-alive, a console that slept, a stop.
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_session.h"
#include "mqtt_client.h"
#include "mqtt_packet.h"

// ---------------------------------------------------------------- clock

static uint64_t now = 1000000;
static uint64_t clock_ms(void) { return now; }

// ---------------------------------------------------------------- broker

#define MAX_RETAINED 64
#define MAX_LOG      512

typedef struct {
    char    topic[160];
    char    payload[32768];   // the discovery: 18.5 KiB
    size_t  len;
    bool    retain;
} Message;

static struct {
    bool     up;              // accepts connections
    uint8_t  refuse_code;     // CONNACK return code (3.1.1's; mapped for a 5.0 client)
    uint8_t  min_version, max_version;   // the protocol levels it speaks (4: 3.1.1, 5: 5.0)
    bool     close_on_unknown;           // another level: closes the stream instead of answering
    unsigned refused_versions;
    uint8_t  version;         // the connected client's
    uint32_t client_max_packet;          // what a 5.0 CONNECT said
    uint16_t server_keepalive;           // 5.0 CONNACK properties (0: not sent)
    uint32_t max_packet;
    bool     no_retain;
    char     reason[64];
    bool     connected;       // a client is connected (CONNECT accepted)
    bool     open;
    bool     answer_pings;
    char     client_id[64], username[64], password[64];
    Message  will;
    bool     has_will;
    char     subs[4][160];
    uint8_t  sub_qos[4];
    size_t   n_subs;
    char     deny[160];       // a filter its access list refuses ("" none)
    Message  retained[MAX_RETAINED];
    size_t   n_retained;
    Message *log;             // every PUBLISH the client sent
    size_t   n_log;
    unsigned pings, disconnects, connects;
    uint8_t  in[65536];       // client -> broker, not parsed yet
    size_t   in_len;
    uint8_t  out[262144];     // broker -> client
    size_t   out_len;
    uint16_t next_id;
} broker;

static void broker_reset(void)
{
    Message *log = broker.log;
    memset(&broker, 0, sizeof(broker));
    broker.log = log;
    broker.up = true;
    broker.answer_pings = true;
    broker.next_id = 1;
    broker.min_version = 4;
    broker.max_version = 5;
}

static void emit(const uint8_t *b, size_t n)
{
    REQUIRE(broker.out_len + n <= sizeof(broker.out));
    memcpy(broker.out + broker.out_len, b, n);
    broker.out_len += n;
}

static void emit_publish(const char *topic, const char *payload, size_t len, bool retain, uint8_t qos)
{
    uint8_t head[256];
    const size_t h = mqtt_encode_publish_head(head, sizeof(head), broker.version ? broker.version : MQTT_V311, topic, len,
                                              qos, retain, false, qos ? broker.next_id++ : 0);
    REQUIRE(h);
    emit(head, h);
    emit((const uint8_t *)payload, len);
}

static bool subscribed(const char *topic, uint8_t *qos)
{
    for (size_t i = 0; i < broker.n_subs; i++)
        if (mqtt_topic_matches(broker.subs[i], topic, strlen(topic))) {
            *qos = broker.sub_qos[i];
            return true;
        }
    return false;
}

static Message *find_retained(const char *topic)
{
    for (size_t i = 0; i < broker.n_retained; i++)
        if (!strcmp(broker.retained[i].topic, topic)) return &broker.retained[i];
    return NULL;
}

// Stores (or deletes, empty payload) a retained message, and delivers it to
// the client when it is subscribed (RETAIN 0: an established subscription).
static void broker_publish(const char *topic, const char *payload, size_t len, bool retain)
{
    if (retain) {
        Message *m = find_retained(topic);
        if (len == 0) {
            if (m) *m = broker.retained[--broker.n_retained];
        } else {
            if (!m) {
                REQUIRE(broker.n_retained < MAX_RETAINED);
                m = &broker.retained[broker.n_retained++];
            }
            snprintf(m->topic, sizeof(m->topic), "%s", topic);
            memcpy(m->payload, payload, len);
            m->payload[len] = '\0';
            m->len = len;
            m->retain = true;
        }
    }
    uint8_t qos;
    if (broker.connected && subscribed(topic, &qos)) emit_publish(topic, payload, len, false, qos);
}

static void broker_drop(bool send_will)
{
    if (broker.connected && send_will && broker.has_will)
        broker_publish(broker.will.topic, broker.will.payload, broker.will.len, broker.will.retain);
    broker.connected = false;
    broker.open = false;
    broker.n_subs = 0;
    broker.in_len = broker.out_len = 0;
}

// 5.0: the broker ends the connection itself, saying why.
static void broker_kick(uint8_t code, const char *reason)
{
    const size_t n = strlen(reason);
    uint8_t d[64] = { 0xE0, (uint8_t)(2 + 3 + n), code, (uint8_t)(3 + n), 0x1F, 0x00, (uint8_t)n };
    memcpy(d + 7, reason, n);
    emit(d, 7 + n);
}

static void read_str(const uint8_t **p, char *out, size_t cap)
{
    const size_t n = ((size_t)(*p)[0] << 8) | (*p)[1];
    REQUIRE(n < cap);
    memcpy(out, *p + 2, n);
    out[n] = '\0';
    *p += 2 + n;
}

static void broker_handle(const MqttPacket *pk)
{
    switch (pk->type) {
    case MQTT_CONNECT: {
        const uint8_t version = pk->body[6];
        const uint8_t *p = pk->body + 7;
        const uint8_t flags = *p++;
        p += 2;   // keep-alive
        if (version == MQTT_V5) {
            // The client's properties: only its maximum packet size.
            const uint8_t n = *p++;
            broker.client_max_packet = 0;
            if (n == 5 && p[0] == 0x27)
                broker.client_max_packet = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
            p += n;
        }
        if (version < broker.min_version || version > broker.max_version) {
            // A broker of another version: "unacceptable protocol version"
            // in 3.1.1's words, or nothing at all.
            broker.refused_versions++;
            if (broker.close_on_unknown) {
                broker_drop(false);
            } else {
                const uint8_t ack[4] = { 0x20, 0x02, 0x00, 0x01 };
                emit(ack, 4);
            }
            break;
        }
        broker.version = version;
        read_str(&p, broker.client_id, sizeof(broker.client_id));
        broker.has_will = flags & 0x04;
        if (broker.has_will) {
            if (version == MQTT_V5) p += 1 + p[0];   // will properties
            read_str(&p, broker.will.topic, sizeof(broker.will.topic));
            char tmp[64];
            read_str(&p, tmp, sizeof(tmp));
            snprintf(broker.will.payload, sizeof(broker.will.payload), "%s", tmp);
            broker.will.len = strlen(tmp);
            broker.will.retain = flags & 0x20;
        }
        broker.username[0] = broker.password[0] = '\0';
        if (flags & 0x80) read_str(&p, broker.username, sizeof(broker.username));
        if (flags & 0x40) read_str(&p, broker.password, sizeof(broker.password));
        if (version == MQTT_V5) {
            uint8_t props[96];
            size_t np = 0;
            if (broker.server_keepalive) {
                props[np++] = 0x13;
                props[np++] = (uint8_t)(broker.server_keepalive >> 8);
                props[np++] = (uint8_t)broker.server_keepalive;
            }
            if (broker.max_packet) {
                props[np++] = 0x27;
                for (int i = 3; i >= 0; i--) props[np++] = (uint8_t)(broker.max_packet >> (8 * i));
            }
            if (broker.no_retain) {
                props[np++] = 0x25;
                props[np++] = 0;
            }
            if (broker.reason[0]) {
                const size_t n = strlen(broker.reason);
                props[np++] = 0x1F;
                props[np++] = 0;
                props[np++] = (uint8_t)n;
                memcpy(props + np, broker.reason, n);
                np += n;
            }
            const uint8_t rc = broker.refuse_code == 0 ? 0 : broker.refuse_code == 4 ? 0x86
                             : broker.refuse_code == 5 ? 0x87 : 0x80;
            uint8_t ack[128] = { 0x20, (uint8_t)(3 + np), 0x00, rc, (uint8_t)np };
            memcpy(ack + 5, props, np);
            emit(ack, 5 + np);
        } else {
            const uint8_t ack[4] = { 0x20, 0x02, 0x00, broker.refuse_code };
            emit(ack, 4);
        }
        broker.connects++;
        broker.connected = broker.refuse_code == 0;
        break;
    }
    case MQTT_PUBLISH: {
        MqttPublish m;
        CHECK(mqtt_decode_publish(pk, broker.version, &m) == MQTT_OK);
        CHECK(m.qos == 0);   // the client publishes QoS 0 only
        char topic[160];
        memcpy(topic, m.topic, m.topic_len);
        topic[m.topic_len] = '\0';
        REQUIRE(broker.n_log < MAX_LOG);
        Message *l = &broker.log[broker.n_log++];
        snprintf(l->topic, sizeof(l->topic), "%s", topic);
        memcpy(l->payload, m.payload, m.payload_len);
        l->payload[m.payload_len] = '\0';
        l->len = m.payload_len;
        l->retain = m.retain;
        broker_publish(topic, (const char *)m.payload, m.payload_len, m.retain);
        break;
    }
    case MQTT_SUBSCRIBE: {
        const uint8_t *p = pk->body;
        const uint16_t id = (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        if (broker.version == MQTT_V5) p += 1 + p[0];   // properties
        uint8_t codes[4];
        size_t n = 0;
        char filters[4][160];
        while (p < pk->body + pk->body_len) {
            read_str(&p, filters[n], sizeof(filters[n]));
            const uint8_t q = *p++;
            if (broker.deny[0] && !strcmp(filters[n], broker.deny)) {
                codes[n++] = broker.version == MQTT_V5 ? 0x87 : 0x80;   // not authorised / failure
                continue;
            }
            // Replace an identical filter, as the spec says.
            size_t k = 0;
            while (k < broker.n_subs && strcmp(broker.subs[k], filters[n])) k++;
            if (k == broker.n_subs) broker.n_subs++;
            snprintf(broker.subs[k], sizeof(broker.subs[k]), "%s", filters[n]);
            broker.sub_qos[k] = q;
            codes[n++] = q;
        }
        if (broker.version == MQTT_V5) {
            uint8_t ack[9] = { 0x90, (uint8_t)(3 + n), (uint8_t)(id >> 8), (uint8_t)id, 0x00 };
            memcpy(ack + 5, codes, n);
            emit(ack, 5 + n);
        } else {
            uint8_t ack[8] = { 0x90, (uint8_t)(2 + n), (uint8_t)(id >> 8), (uint8_t)id };
            memcpy(ack + 4, codes, n);
            emit(ack, 4 + n);
        }
        // Retained messages matching a new subscription, RETAIN 1.
        for (size_t f = 0; f < n; f++)
            for (size_t i = 0; codes[f] < 0x80 && i < broker.n_retained; i++)
                if (mqtt_topic_matches(filters[f], broker.retained[i].topic, strlen(broker.retained[i].topic)))
                    emit_publish(broker.retained[i].topic, broker.retained[i].payload, broker.retained[i].len, true,
                                 codes[f]);
        break;
    }
    case MQTT_PUBACK:
        break;
    case MQTT_PINGREQ:
        broker.pings++;
        if (broker.answer_pings) {
            const uint8_t r[2] = { 0xD0, 0x00 };
            emit(r, 2);
        }
        break;
    case MQTT_DISCONNECT:
        broker.disconnects++;
        broker_drop(false);
        break;
    default:
        CHECK(!"unexpected packet from the client");
    }
}

static void broker_feed(void)
{
    for (;;) {
        MqttPacket pk;
        if (mqtt_parse(broker.in, broker.in_len, sizeof(broker.in), &pk) != MQTT_OK) return;
        const size_t size = pk.size;
        broker_handle(&pk);
        if (!broker.open) return;
        memmove(broker.in, broker.in + size, broker.in_len - size);
        broker.in_len -= size;
    }
}

// ---------------------------------------------------------------- the client's stream

static int io_open(void *ctx, const char *host, uint16_t port, int timeout_ms, char *err, size_t err_size)
{
    (void)ctx;
    (void)timeout_ms;
    CHECK(!strcmp(host, "broker.lan"));
    CHECK(port == 1883);
    if (!broker.up) {
        snprintf(err, err_size, "connection refused");
        return -1;
    }
    broker.open = true;
    broker.in_len = broker.out_len = 0;
    return 0;
}

static int io_write(void *ctx, const void *buf, size_t len, int timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    if (!broker.open) return -1;
    REQUIRE(broker.in_len + len <= sizeof(broker.in));
    memcpy(broker.in + broker.in_len, buf, len);
    broker.in_len += len;
    broker_feed();
    return 0;
}

static long io_read(void *ctx, void *buf, size_t len, int timeout_ms)
{
    (void)ctx;
    if (!broker.open) return -1;
    if (broker.out_len == 0) {
        now += (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);   // waited that long
        return 0;
    }
    const size_t n = broker.out_len < len ? broker.out_len : len;
    memcpy(buf, broker.out, n);
    memmove(broker.out, broker.out + n, broker.out_len - n);
    broker.out_len -= n;
    return (long)n;
}

static void io_close(void *ctx)
{
    (void)ctx;
    if (broker.open) broker_drop(true);
}


// ---------------------------------------------------------------- the session's host

static BlSession session;

static struct {
    int      subscribes, losts;
    int      sub_fail;            // the subscription callback fails
    uint64_t posix;               // the console's clock (0: unknown)
    char     log[320];
    size_t   got;                 // messages handed over
    char     got_topic[160], got_payload[256];
    bool     got_retain;
} host;

static int h_subscribe(void *ctx)
{
    (void)ctx;
    host.subscribes++;
    if (host.sub_fail) return -1;
    const char *filters[2] = { "home/c1/+/set", "home/c1/optional" };
    const uint8_t qos[2] = { 1, 0 };
    return bl_session_subscribe(&session, filters, qos, 2, 1) < 0 ? -1 : 0;
}

static void h_lost(void *ctx)
{
    (void)ctx;
    host.losts++;
}

static uint64_t h_posix(void *ctx)
{
    (void)ctx;
    return host.posix;
}

static void h_log(void *ctx, const char *line)
{
    (void)ctx;
    snprintf(host.log, sizeof(host.log), "%s", line);
}

static void on_message(void *user, const MqttPublish *m)
{
    CHECK(user == &host);
    host.got++;
    snprintf(host.got_topic, sizeof(host.got_topic), "%.*s", (int)m->topic_len, m->topic);
    snprintf(host.got_payload, sizeof(host.got_payload), "%.*s", (int)m->payload_len, (const char *)m->payload);
    host.got_retain = m->retain;
}

static BlSessionConf conf;

static void setup(uint8_t version)
{
    broker_reset();
    memset(&host, 0, sizeof(host));
    memset(&conf, 0, sizeof(conf));
    conf.host = "broker.lan";
    conf.port = 1883;
    conf.username = "parent";
    conf.password = "secret";
    conf.version = version;
    conf.base = "home/c1";
    conf.client_id = "bl-c1-test";
    const BlIo io = { NULL, io_open, io_write, io_read, io_close };
    const BlSessionHost h = { NULL, h_subscribe, h_lost, h_posix, h_log };
    bl_session_init(&session, &conf, io, &h, clock_ms, on_message, &host);
}

// One turn as a caller drives it: connect when due, then wait for the broker.
static void turns(int n)
{
    for (int i = 0; i < n; i++) {
        bl_session_woke(&session);
        bool connected;
        const int sleep = bl_session_connect_due(&session, 50, &connected);
        if (sleep >= 0) {
            now += (uint64_t)sleep;
            continue;
        }
        if (!bl_session_check(&session)) continue;
        bl_session_poll(&session, 50);
    }
}

static const Message *last_on(const char *topic)
{
    for (size_t i = broker.n_log; i-- > 0;)
        if (!strcmp(broker.log[i].topic, topic)) return &broker.log[i];
    return NULL;
}

// ---------------------------------------------------------------- tests

static void test_connect_and_publish(void)
{
    setup(0);
    turns(1);
    CHECK(bl_session_online(&session) && session.status.state == BlLink_Online);
    CHECK(session.status.connects == 1 && session.status.protocol == MQTT_V5 && !session.status.error[0]);
    CHECK(!strcmp(broker.client_id, "bl-c1-test"));
    CHECK(!strcmp(broker.username, "parent") && !strcmp(broker.password, "secret"));
    // The last will: offline, retained, on the availability topic.
    CHECK(broker.has_will && !strcmp(broker.will.topic, "home/c1/availability"));
    CHECK(!strcmp(broker.will.payload, "offline") && broker.will.retain);
    // Then online, retained, and the caller's subscriptions.
    const Message *avail = find_retained("home/c1/availability");
    CHECK(avail && !strcmp(avail->payload, "online"));
    CHECK(host.subscribes == 1 && broker.n_subs == 2);

    // Publishing under the base, counted.
    CHECK(bl_session_publish(&session, "state", "{\"a\":1}", 7, true) == 0);
    const Message *m = last_on("home/c1/state");
    CHECK(m && m->retain && !strcmp(m->payload, "{\"a\":1}"));
    CHECK(session.status.publishes == 1 && session.status.last_publish_ms == now);
    // Another root, not counted; "online" again.
    CHECK(bl_session_publish_topic(&session, "other/topic", "x", 1, false) == 0 && last_on("other/topic"));
    CHECK(bl_session_announce(&session) == 0 && session.status.publishes == 1);

    // What the subscriptions bring goes to the caller.
    broker_publish("home/c1/limit/set", "90", 2, true);
    turns(1);
    CHECK(host.got == 1 && !strcmp(host.got_topic, "home/c1/limit/set") && !strcmp(host.got_payload, "90"));

    // A clean stop: offline, DISCONNECT, no last will; off until the next turn.
    const unsigned disconnects = broker.disconnects;
    bl_session_stop(&session);
    CHECK(!bl_session_online(&session) && session.status.state == BlLink_Off);
    CHECK(broker.disconnects == disconnects + 1);
    avail = find_retained("home/c1/availability");
    CHECK(!strcmp(avail->payload, "offline"));
    CHECK(bl_session_publish(&session, "state", "{}", 2, true) < 0);
    // A retained order waiting on the broker comes with the next connection.
    turns(1);
    CHECK(bl_session_online(&session) && host.got == 2 && host.got_retain);
}

static void test_subscriptions(void)
{
    // An optional filter refused: still online, and the caller is told how many.
    setup(0);
    snprintf(broker.deny, sizeof(broker.deny), "home/c1/optional");
    turns(1);
    CHECK(bl_session_online(&session) && broker.n_subs == 1);
    const char *filters[2] = { "home/c1/+/set", "home/c1/optional" };
    const uint8_t qos[2] = { 1, 0 };
    CHECK(bl_session_subscribe(&session, filters, qos, 2, 1) == 1);

    // A required one refused, or the caller failing: the connection fails.
    setup(0);
    snprintf(broker.deny, sizeof(broker.deny), "home/c1/+/set");
    turns(1);
    CHECK(!bl_session_online(&session) && session.status.state == BlLink_Waiting && host.losts == 1);
    setup(0);
    host.sub_fail = 1;
    turns(1);
    CHECK(!bl_session_online(&session) && host.subscribes == 1 && host.losts == 1);
    CHECK(strstr(session.status.error, "the subscriptions failed"));
    const Message *avail = find_retained("home/c1/availability");
    CHECK(avail && !strcmp(avail->payload, "offline"));   // the last will
}

static void test_failures_and_pause(void)
{
    // Refused credentials: the reason, then a pause before the next attempt.
    setup(0);
    broker.refuse_code = 4;
    turns(1);
    CHECK(!bl_session_online(&session) && session.status.state == BlLink_Waiting);
    CHECK(strstr(session.status.error, "bad user name or password") && strstr(host.log, "remote link: "));
    CHECK(session.backoff_ms == BL_BACKOFF_MIN_MS && session.status.next_attempt_ms == now + BL_BACKOFF_MIN_MS);
    const unsigned attempts = broker.connects;
    bool connected;
    CHECK(bl_session_connect_due(&session, 50, &connected) == 50 && !connected);
    CHECK(broker.connects == attempts);   // not before the pause
    broker.refuse_code = 0;
    now += 5000;
    turns(1);
    CHECK(bl_session_online(&session) && session.backoff_ms == 0);

    // The broker drops the connection: its last will says offline; the next
    // turn after the shortest pause connects again and says online.
    broker_drop(true);
    turns(1);
    const Message *avail = find_retained("home/c1/availability");
    CHECK(avail && !strcmp(avail->payload, "offline") && !bl_session_online(&session));
    now += 6000;
    turns(2);
    CHECK(bl_session_online(&session) && !strcmp(find_retained("home/c1/availability")->payload, "online"));

    // Unreachable: the pause doubles, up to 5 minutes.
    broker_drop(false);
    broker.up = false;
    turns(1);
    CHECK(!bl_session_online(&session) && session.status.state == BlLink_Waiting);
    for (int i = 0; i < 12; i++) {
        now += 400000;
        turns(1);
    }
    CHECK(session.backoff_ms == BL_BACKOFF_MAX_MS && strstr(session.status.error, "connection refused"));
    // New settings: the next attempt at once.
    bl_session_retry_now(&session);
    CHECK(session.backoff_ms == 0 && session.status.next_attempt_ms == 0 && session.status.state == BlLink_Waiting);
    broker.up = true;
    turns(1);
    CHECK(bl_session_online(&session));
}

static void test_versions(void)
{
    // Version 0: 5.0 first, 3.1.1 when the broker refuses it, kept from then on.
    setup(0);
    broker.max_version = 4;
    turns(1);
    CHECK(bl_session_online(&session) && broker.version == MQTT_V311 && session.proto_ok == MQTT_V311);
    CHECK(session.status.protocol == MQTT_V311 && broker.refused_versions == 1);
    bl_session_stop(&session);
    turns(1);
    CHECK(bl_session_online(&session) && broker.refused_versions == 1);   // straight to 3.1.1
    // Another broker: 5.0 again.
    bl_session_stop(&session);
    broker.max_version = 5;
    bl_session_configure(&session, &conf, true);
    turns(1);
    CHECK(bl_session_online(&session) && broker.version == MQTT_V5 && session.proto_ok == MQTT_V5);

    // A version asked for: no fallback.
    setup(MQTT_V5);
    broker.max_version = 4;
    broker.close_on_unknown = true;
    turns(1);
    CHECK(!bl_session_online(&session) && broker.refused_versions == 1);
}

static void test_keepalive(void)
{
    setup(0);
    turns(1);
    const unsigned pings = broker.pings;
    // Nothing sent for 3/4 of the keep-alive: a PINGREQ.
    for (int i = 0; i < 30 && broker.pings == pings; i++) {
        now += 1000;
        bl_session_poll(&session, 0);
    }
    CHECK(broker.pings == pings + 1 && bl_session_online(&session));
    // The broker stops answering: closed after the keep-alive, and told lost.
    broker.answer_pings = false;
    for (int i = 0; i < 90 && bl_session_online(&session); i++) {
        now += 1000;
        bl_session_poll(&session, 0);
    }
    CHECK(!bl_session_online(&session) && strstr(session.status.error, "stopped answering"));
    CHECK(host.losts == 1 && session.backoff_ms == BL_BACKOFF_MIN_MS);
}

// The console slept: its clock moved on by more than the broker keeps a
// silent connection. Closed without a word before anything is written.
static void test_woke(void)
{
    setup(0);
    host.posix = 1791000000;
    turns(1);
    CHECK(bl_session_online(&session) && bl_session_publish(&session, "names", "{}", 2, true) == 0);
    const unsigned connects = broker.connects, disconnects = broker.disconnects;
    const size_t sent = broker.n_log;
    host.posix += 40;   // within the keep-alive
    CHECK(bl_session_publish(&session, "names", "{}", 2, true) == 0 && bl_session_online(&session));
    host.posix += 600;   // ten minutes asleep
    CHECK(bl_session_publish(&session, "names", "{}", 2, true) < 0);
    CHECK(!bl_session_online(&session) && strstr(session.status.error, "slept") && session.backoff_ms == 0);
    CHECK(broker.n_log == sent + 1 && broker.disconnects == disconnects);   // not written, no DISCONNECT
    turns(1);
    CHECK(bl_session_online(&session) && broker.connects == connects + 1);
    // No clock: never asked.
    setup(0);
    turns(1);
    CHECK(!bl_session_woke(&session) && bl_session_online(&session));
}

// 5.0: the broker ends the connection saying why; the reason is the status.
static void test_kicked(void)
{
    setup(0);
    turns(1);
    broker_kick(0x8E, "session taken over");
    turns(1);
    CHECK(!bl_session_online(&session) && strstr(session.status.error, "session taken over"));
    CHECK(host.losts == 1 && strstr(host.log, "session taken over"));
}

typedef struct {
    const char *data;
    size_t      len, at, piece;
} Reader;

static size_t read_piece(void *ctx, void *buf, size_t cap)
{
    Reader *r = (Reader *)ctx;
    size_t n = r->len - r->at;
    if (n > cap) n = cap;
    if (n > r->piece) n = r->piece;
    memcpy(buf, r->data + r->at, n);
    r->at += n;
    return n;
}

static void test_publish_stream(void)
{
    setup(0);
    turns(1);
    static char icon[20000];
    for (size_t i = 0; i < sizeof(icon); i++) icon[i] = (char)(i * 7 + 1);
    char buf[4096];
    Reader r = { icon, sizeof(icon), 0, 1500 };
    CHECK(bl_session_publish_stream(&session, "icon/1", sizeof(icon), true, read_piece, &r, buf, sizeof(buf)) == 0);
    const Message *m = last_on("home/c1/icon/1");
    CHECK(m && m->retain && m->len == sizeof(icon) && !memcmp(m->payload, icon, sizeof(icon)));
    CHECK(session.status.publishes == 1 && bl_session_online(&session));
    // A file that comes short: half a packet went out, the connection is
    // closed and connects again at the next turn; nothing was published.
    Reader cut = { icon, 5000, 0, 1500 };
    CHECK(bl_session_publish_stream(&session, "icon/2", 6000, true, read_piece, &cut, buf, sizeof(buf)) < 0);
    CHECK(!bl_session_online(&session) && strstr(session.status.error, "cut short"));
    turns(1);
    CHECK(bl_session_online(&session) && !last_on("home/c1/icon/2"));
    // Offline: refused at once, nothing read.
    bl_session_stop(&session);
    Reader none = { icon, 10, 0, 10 };
    CHECK(bl_session_publish_stream(&session, "icon/3", 10, true, read_piece, &none, buf, sizeof(buf)) < 0 && none.at == 0);
}

int main(void)
{
    static Message log[MAX_LOG];
    broker.log = log;
    test_connect_and_publish();
    test_subscriptions();
    test_failures_and_pause();
    test_versions();
    test_keepalive();
    test_woke();
    test_kicked();
    test_publish_stream();
    return CHECK_DONE("session: all tests passed");
}
