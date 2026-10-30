// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "mqtt_packet.h"

#include <string.h>

// Bytes the remaining length takes (1..4), 0 when it is too large.
static size_t varint_size(size_t n)
{
    if (n < 128u) return 1;
    if (n < 16384u) return 2;
    if (n < 2097152u) return 3;
    if (n <= MQTT_MAX_REMAINING) return 4;
    return 0;
}

static size_t put_varint(uint8_t *p, size_t n)
{
    size_t i = 0;
    do {
        uint8_t b = (uint8_t)(n % 128u);
        n /= 128u;
        if (n > 0) b |= 0x80u;
        p[i++] = b;
    } while (n > 0);
    return i;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// A variable byte integer at `p` (at most `len` bytes there): its value and
// size, false when malformed or cut short.
static bool get_varint(const uint8_t *p, size_t len, uint32_t *value, size_t *used)
{
    uint32_t v = 0, mult = 1;
    for (size_t i = 0; i < 4; i++) {
        if (i >= len) return false;
        v += (uint32_t)(p[i] & 0x7Fu) * mult;
        if (!(p[i] & 0x80u)) {
            *value = v;
            *used = i + 1;
            return true;
        }
        mult *= 128u;
    }
    return false;
}

// ---- 5.0 properties ----

typedef void (*PropFn)(void *ctx, uint8_t id, const uint8_t *value, size_t len);

// The size of a property's value at `p`, from its identifier's type; 0 when
// unknown or cut short.
static size_t prop_size(uint8_t id, const uint8_t *p, size_t left)
{
    switch (id) {
    // byte
    case 0x01: case 0x17: case 0x19: case 0x24: case 0x25: case 0x28: case 0x29: case 0x2A:
        return left >= 1 ? 1 : 0;
    // two byte integer
    case 0x13: case 0x21: case 0x22: case 0x23:
        return left >= 2 ? 2 : 0;
    // four byte integer
    case 0x02: case 0x11: case 0x18: case 0x27:
        return left >= 4 ? 4 : 0;
    // variable byte integer (subscription identifier)
    case 0x0B: {
        uint32_t v;
        size_t used;
        return get_varint(p, left, &v, &used) ? used : 0;
    }
    // UTF-8 string, binary data
    case 0x03: case 0x08: case 0x09: case 0x12: case 0x15: case 0x16: case 0x1A: case 0x1C: case 0x1F: {
        if (left < 2) return 0;
        const size_t n = 2 + (size_t)get16(p);
        return n <= left ? n : 0;
    }
    // string pair (user property)
    case 0x26: {
        if (left < 2) return 0;
        size_t n = 2 + (size_t)get16(p);
        if (n + 2 > left) return 0;
        n += 2 + (size_t)get16(p + n);
        return n <= left ? n : 0;
    }
    default:
        return 0;
    }
}

// The property list at `p` (its length first): calls `fn` for each property
// and returns the bytes the list takes, 0 when malformed.
static size_t props_walk(const uint8_t *p, size_t left, PropFn fn, void *ctx)
{
    uint32_t len;
    size_t used;
    if (!get_varint(p, left, &len, &used) || len > left - used) return 0;
    const uint8_t *q = p + used, *end = q + len;
    while (q < end) {
        const uint8_t id = *q++;   // every identifier fits in one byte
        const size_t n = prop_size(id, q, (size_t)(end - q));
        if (n == 0) return 0;
        if (fn) fn(ctx, id, q, n);
        q += n;
    }
    return used + len;
}

static void copy_reason(char *out, size_t cap, const uint8_t *value, size_t len)
{
    // A UTF-8 string: its length, then its bytes.
    size_t n = len >= 2 ? get16(value) : 0;
    if (n > len - 2) n = len - 2;
    if (n >= cap) n = cap - 1;
    memcpy(out, value + 2, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)out[i] < 0x20) out[i] = ' ';
}

// A length-prefixed string: 2 + n bytes.
static size_t put_str(uint8_t *p, const void *s, size_t n)
{
    put16(p, (uint16_t)n);
    if (n) memcpy(p + 2, s, n);
    return 2 + n;
}

// Room for a packet of `remaining` bytes after its fixed header; writes the
// header and returns its size, 0 when it does not fit.
static size_t head(uint8_t *buf, size_t cap, uint8_t first, size_t remaining)
{
    const size_t vs = varint_size(remaining);
    if (vs == 0 || cap < 1 + vs || cap - 1 - vs < remaining) return 0;
    buf[0] = first;
    return 1 + put_varint(buf + 1, remaining);
}

bool mqtt_topic_valid(const char *topic)
{
    if (!topic || !*topic) return false;
    const size_t n = strlen(topic);
    if (n > 65535u) return false;
    for (size_t i = 0; i < n; i++)
        if (topic[i] == '+' || topic[i] == '#') return false;
    return true;
}

size_t mqtt_encode_connect(uint8_t *buf, size_t cap, const MqttConnect *c)
{
    if (!c || !c->client_id) return 0;
    const size_t id_len = strlen(c->client_id);
    if (id_len > 65535u) return 0;
    const bool user = c->username != NULL;
    const bool pass = user && c->password != NULL;
    const bool will = c->will_topic != NULL;
    if (will && (!mqtt_topic_valid(c->will_topic) || c->will_qos > 1 || c->will_len > 65535u)) return 0;

    const bool v5 = c->version == MQTT_V5;
    // 5.0: the CONNECT's properties (the maximum packet size, when said) and
    // the will's (none).
    const size_t props = v5 ? 1 + (c->max_packet ? 5u : 0u) : 0;
    size_t remaining = 10 + props + 2 + id_len;   // variable header + client id
    if (will) remaining += (v5 ? 1u : 0u) + 2 + strlen(c->will_topic) + 2 + c->will_len;
    if (user) {
        if (strlen(c->username) > 65535u) return 0;
        remaining += 2 + strlen(c->username);
    }
    if (pass) {
        if (strlen(c->password) > 65535u) return 0;
        remaining += 2 + strlen(c->password);
    }

    size_t at = head(buf, cap, (uint8_t)(MQTT_CONNECT << 4), remaining);
    if (!at) return 0;
    at += put_str(buf + at, "MQTT", 4);
    buf[at++] = v5 ? MQTT_V5 : MQTT_V311;
    uint8_t flags = 0;
    if (c->clean_session) flags |= 0x02;
    if (will) {
        flags |= 0x04;
        flags |= (uint8_t)(c->will_qos << 3);
        if (c->will_retain) flags |= 0x20;
    }
    if (pass) flags |= 0x40;
    if (user) flags |= 0x80;
    buf[at++] = flags;
    put16(buf + at, c->keepalive_s);
    at += 2;
    if (v5) {
        buf[at++] = (uint8_t)(props - 1);
        if (c->max_packet) {
            buf[at++] = 0x27;   // maximum packet size
            put32(buf + at, c->max_packet);
            at += 4;
        }
    }
    at += put_str(buf + at, c->client_id, id_len);
    if (will) {
        if (v5) buf[at++] = 0;   // will properties: none
        at += put_str(buf + at, c->will_topic, strlen(c->will_topic));
        at += put_str(buf + at, c->will_payload, c->will_len);
    }
    if (user) at += put_str(buf + at, c->username, strlen(c->username));
    if (pass) at += put_str(buf + at, c->password, strlen(c->password));
    return at;
}

size_t mqtt_encode_publish_head(uint8_t *buf, size_t cap, uint8_t version, const char *topic, size_t payload_len,
                                uint8_t qos, bool retain, bool dup, uint16_t packet_id)
{
    if (!mqtt_topic_valid(topic) || qos > 1) return 0;
    if (qos == 1 && packet_id == 0) return 0;
    const size_t topic_len = strlen(topic);
    const size_t vh = 2 + topic_len + (qos ? 2u : 0u) + (version == MQTT_V5 ? 1u : 0u);
    if (payload_len > MQTT_MAX_REMAINING - vh) return 0;
    const size_t remaining = vh + payload_len;
    const size_t vs = varint_size(remaining);
    if (vs == 0 || cap < 1 + vs + vh) return 0;
    uint8_t first = (uint8_t)(MQTT_PUBLISH << 4);
    if (dup) first |= 0x08;
    first |= (uint8_t)(qos << 1);
    if (retain) first |= 0x01;
    buf[0] = first;
    size_t at = 1 + put_varint(buf + 1, remaining);
    at += put_str(buf + at, topic, topic_len);
    if (qos) {
        put16(buf + at, packet_id);
        at += 2;
    }
    if (version == MQTT_V5) buf[at++] = 0;   // properties: none
    return at;
}

size_t mqtt_encode_puback(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    if (cap < 4) return 0;
    buf[0] = (uint8_t)(MQTT_PUBACK << 4);
    buf[1] = 2;
    put16(buf + 2, packet_id);
    return 4;
}

size_t mqtt_encode_subscribe(uint8_t *buf, size_t cap, uint8_t version, uint16_t packet_id,
                             const char *const *topics, const uint8_t *qos, size_t count)
{
    if (count == 0 || packet_id == 0 || !topics) return 0;
    const bool v5 = version == MQTT_V5;
    size_t remaining = 2 + (v5 ? 1u : 0u);
    for (size_t i = 0; i < count; i++) {
        if (!topics[i] || !*topics[i]) return 0;
        const size_t n = strlen(topics[i]);
        if (n > 65535u || (qos && qos[i] > 1)) return 0;
        remaining += 2 + n + 1;
    }
    // SUBSCRIBE carries the reserved flags 0010.
    size_t at = head(buf, cap, (uint8_t)((MQTT_SUBSCRIBE << 4) | 0x02), remaining);
    if (!at) return 0;
    put16(buf + at, packet_id);
    at += 2;
    if (v5) buf[at++] = 0;   // properties: none
    for (size_t i = 0; i < count; i++) {
        at += put_str(buf + at, topics[i], strlen(topics[i]));
        // 5.0's subscription options: the QoS, and No Local, Retain As
        // Published and Retain Handling left at 0 (as 3.1.1 behaves).
        buf[at++] = qos ? qos[i] : 0;
    }
    return at;
}

size_t mqtt_encode_empty(uint8_t *buf, size_t cap, MqttType type)
{
    if (cap < 2 || (type != MQTT_PINGREQ && type != MQTT_DISCONNECT)) return 0;
    buf[0] = (uint8_t)(type << 4);
    buf[1] = 0;
    return 2;
}

MqttResult mqtt_parse(const uint8_t *buf, size_t len, size_t max_size, MqttPacket *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 2) return MQTT_INCOMPLETE;
    size_t remaining = 0, mult = 1, i = 1;
    for (;;) {
        if (i >= len) return MQTT_INCOMPLETE;
        if (i > 4) return MQTT_MALFORMED;   // a fifth length byte
        const uint8_t b = buf[i++];
        remaining += (size_t)(b & 0x7Fu) * mult;
        if (!(b & 0x80u)) break;
        mult *= 128u;
    }
    const uint8_t type = buf[0] >> 4;
    if (type == 0 || type == 15) return MQTT_MALFORMED;
    if (remaining > max_size || i + remaining > max_size) return MQTT_TOO_LARGE;
    if (len - i < remaining) return MQTT_INCOMPLETE;
    out->type = type;
    out->flags = buf[0] & 0x0Fu;
    out->body = buf + i;
    out->body_len = remaining;
    out->size = i + remaining;
    return MQTT_OK;
}

static void connack_prop(void *ctx, uint8_t id, const uint8_t *v, size_t len)
{
    MqttConnack *out = (MqttConnack *)ctx;
    switch (id) {
    case 0x13:
        out->server_keepalive = get16(v);
        out->has_server_keepalive = true;
        break;
    case 0x27: out->max_packet = get32(v); break;
    case 0x25: out->retain_unavailable = v[0] == 0; break;
    case 0x28: out->wildcard_unavailable = v[0] == 0; break;
    case 0x1F: copy_reason(out->reason, sizeof(out->reason), v, len); break;
    default: break;
    }
}

MqttResult mqtt_decode_connack(const MqttPacket *p, uint8_t version, MqttConnack *out)
{
    memset(out, 0, sizeof(*out));
    if (p->type != MQTT_CONNACK || p->flags != 0 || p->body_len < 2) return MQTT_MALFORMED;
    if (p->body[0] & 0xFEu) return MQTT_MALFORMED;
    out->session_present = p->body[0] & 1u;
    out->return_code = p->body[1];
    // A 3.1.1 answer has no properties: what a 3.1.1 broker sends a 5.0
    // CONNECT ("unacceptable protocol version").
    if (p->body_len == 2) {
        out->v311 = true;
        return MQTT_OK;
    }
    if (version != MQTT_V5) return MQTT_MALFORMED;
    const size_t n = props_walk(p->body + 2, p->body_len - 2, connack_prop, out);
    if (n == 0 || 2 + n != p->body_len) return MQTT_MALFORMED;
    return MQTT_OK;
}

MqttResult mqtt_decode_publish(const MqttPacket *p, uint8_t version, MqttPublish *out)
{
    memset(out, 0, sizeof(*out));
    if (p->type != MQTT_PUBLISH) return MQTT_MALFORMED;
    const uint8_t qos = (p->flags >> 1) & 3u;
    if (qos > 2) return MQTT_MALFORMED;
    if (p->body_len < 2) return MQTT_MALFORMED;
    const size_t topic_len = get16(p->body);
    const size_t vh = 2 + topic_len + (qos ? 2u : 0u);
    if (topic_len == 0 || vh > p->body_len) return MQTT_MALFORMED;
    const char *topic = (const char *)p->body + 2;
    for (size_t i = 0; i < topic_len; i++)
        if (topic[i] == '\0' || topic[i] == '+' || topic[i] == '#') return MQTT_MALFORMED;
    out->topic = topic;
    out->topic_len = topic_len;
    out->qos = qos;
    out->retain = p->flags & 1u;
    out->dup = (p->flags & 8u) != 0;
    if (qos) {
        out->packet_id = get16(p->body + 2 + topic_len);
        if (out->packet_id == 0) return MQTT_MALFORMED;
    }
    size_t at = vh;
    if (version == MQTT_V5) {
        // Its properties (message expiry, content type …) mean nothing here.
        // No topic alias: the CONNECT allowed none, so a topic is always there.
        const size_t n = props_walk(p->body + at, p->body_len - at, NULL, NULL);
        if (n == 0) return MQTT_MALFORMED;
        at += n;
    }
    out->payload = p->body + at;
    out->payload_len = p->body_len - at;
    return MQTT_OK;
}

MqttResult mqtt_decode_puback(const MqttPacket *p, uint8_t version, uint16_t *packet_id)
{
    if (p->type != MQTT_PUBACK || p->flags != 0 || p->body_len < 2) return MQTT_MALFORMED;
    // 5.0 may add a reason code and properties.
    if (p->body_len != 2 && version != MQTT_V5) return MQTT_MALFORMED;
    *packet_id = get16(p->body);
    return MQTT_OK;
}

MqttResult mqtt_decode_suback(const MqttPacket *p, uint8_t version, uint16_t *packet_id, uint8_t *codes, size_t max,
                              size_t *count)
{
    *count = 0;
    if (p->type != MQTT_SUBACK || p->flags != 0 || p->body_len < 3) return MQTT_MALFORMED;
    *packet_id = get16(p->body);
    size_t at = 2;
    if (version == MQTT_V5) {
        const size_t n = props_walk(p->body + at, p->body_len - at, NULL, NULL);
        if (n == 0 || at + n >= p->body_len) return MQTT_MALFORMED;
        at += n;
    }
    const size_t n = p->body_len - at;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p->body[at + i];
        // 3.1.1: 0x80 is the only failure; 5.0 has a reason code for each.
        if (c > 2 && (version == MQTT_V5 ? c < 0x80u : c != 0x80u)) return MQTT_MALFORMED;
        if (i < max) codes[i] = c;
    }
    *count = n < max ? n : max;
    return MQTT_OK;
}

typedef struct {
    char  *reason;
    size_t cap;
} ReasonCtx;

static void disconnect_prop(void *ctx, uint8_t id, const uint8_t *v, size_t len)
{
    ReasonCtx *r = (ReasonCtx *)ctx;
    if (id == 0x1F && r->reason && r->cap) copy_reason(r->reason, r->cap, v, len);
}

MqttResult mqtt_decode_disconnect(const MqttPacket *p, uint8_t *reason_code, char *reason, size_t reason_cap)
{
    if (reason && reason_cap) reason[0] = '\0';
    *reason_code = 0;
    if (p->type != MQTT_DISCONNECT || p->flags != 0) return MQTT_MALFORMED;
    if (p->body_len == 0) return MQTT_OK;   // a normal disconnection
    *reason_code = p->body[0];
    if (p->body_len == 1) return MQTT_OK;
    ReasonCtx ctx = { reason, reason_cap };
    const size_t n = props_walk(p->body + 1, p->body_len - 1, disconnect_prop, &ctx);
    return n && 1 + n == p->body_len ? MQTT_OK : MQTT_MALFORMED;
}

const char *mqtt_connack_text(uint8_t rc)
{
    switch (rc) {
        case 0: return "accepted";
        case 1: return "the broker does not speak this MQTT version";
        case 2: return "client identifier refused";
        case 3: return "broker unavailable";
        case 4: return "bad user name or password";
        case 5: return "not authorised";
        default: return "refused";
    }
}

const char *mqtt_reason_text(uint8_t code)
{
    switch (code) {
        case 0x00: return "success";
        case 0x04: return "disconnected with the last will";
        case 0x80: return "unspecified error";
        case 0x81: return "malformed packet";
        case 0x82: return "protocol error";
        case 0x83: return "implementation-specific error";
        case 0x84: return "the broker does not speak this MQTT version";
        case 0x85: return "client identifier refused";
        case 0x86: return "bad user name or password";
        case 0x87: return "not authorised";
        case 0x88: return "broker unavailable";
        case 0x89: return "broker busy";
        case 0x8A: return "banned";
        case 0x8B: return "the broker is shutting down";
        case 0x8C: return "bad authentication method";
        case 0x8D: return "keep-alive timeout";
        case 0x8E: return "session taken over by another client";
        case 0x8F: return "topic filter refused";
        case 0x90: return "topic name refused";
        case 0x93: return "receive maximum exceeded";
        case 0x94: return "topic alias invalid";
        case 0x95: return "packet too large";
        case 0x97: return "quota exceeded";
        case 0x98: return "administrative action";
        case 0x99: return "payload format invalid";
        case 0x9A: return "retained messages not supported";
        case 0x9B: return "QoS not supported";
        case 0x9C: return "use another broker";
        case 0x9D: return "the broker moved";
        case 0x9E: return "shared subscriptions not supported";
        case 0x9F: return "connection rate exceeded";
        case 0xA0: return "maximum connection time";
        case 0xA1: return "subscription identifiers not supported";
        case 0xA2: return "wildcard subscriptions not supported";
        default: return "refused";
    }
}

bool mqtt_topic_matches(const char *filter, const char *topic, size_t len)
{
    size_t t = 0;
    const char *f = filter;
    for (;;) {
        if (*f == '#') return f[1] == '\0';   // matches the rest, the parent level included
        // One level of the filter against one level of the topic.
        const char *f_end = f;
        while (*f_end && *f_end != '/') f_end++;
        size_t t_end = t;
        while (t_end < len && topic[t_end] != '/') t_end++;
        const size_t f_n = (size_t)(f_end - f);
        if (!(f_n == 1 && *f == '+')) {
            if (f_n != t_end - t || memcmp(f, topic + t, f_n) != 0) return false;
        }
        const bool f_more = *f_end == '/';
        const bool t_more = t_end < len;
        if (!f_more && !t_more) return true;
        if (!f_more) return false;
        f = f_end + 1;
        if (!t_more) return *f == '#' && f[1] == '\0';   // "a/#" matches "a"
        t = t_end + 1;
    }
}
