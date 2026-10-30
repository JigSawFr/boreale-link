// Host tests for what needs neither a console nor a network: the JSON
// writer, the MQTT 5.0 / 3.1.1 codec and topic filters, and the flat files'
// key=value reader.
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_json.h"
#include "bl_kv.h"
#include "mqtt_packet.h"

static void test_json(void)
{
    char buf[256];
    BlJson j;
    bl_json_init(&j, buf, sizeof(buf));
    bl_json_obj(&j);
    bl_json_kstr(&j, "a", "x\"y\\z\n\t\x01");
    bl_json_kint(&j, "n", -42);
    bl_json_kbool(&j, "t", true);
    bl_json_knull(&j, "z");
    bl_json_key(&j, "arr");
    bl_json_arr(&j);
    bl_json_int(&j, 1);
    bl_json_obj(&j);
    bl_json_obj_end(&j);
    bl_json_hex64(&j, 0x0100000000010000ULL);
    bl_json_arr_end(&j);
    bl_json_kint_or_null(&j, "maybe", false, 5);
    bl_json_members(&j, "\"m\":1,\"k\":2");
    bl_json_obj_end(&j);
    const size_t n = bl_json_end(&j);
    CHECK(n == strlen(buf));
    CHECK(!strcmp(buf, "{\"a\":\"x\\\"y\\\\z\\n\\t\\u0001\",\"n\":-42,\"t\":true,\"z\":null,"
                        "\"arr\":[1,{},\"0100000000010000\"],\"maybe\":null,\"m\":1,\"k\":2}"));

    // Too small: 0, and the buffer stays a valid C string.
    char small[8];
    bl_json_init(&j, small, sizeof(small));
    bl_json_obj(&j);
    bl_json_kstr(&j, "long", "value");
    bl_json_obj_end(&j);
    CHECK(bl_json_end(&j) == 0);
    CHECK(strlen(small) < sizeof(small));

    // Unbalanced: 0.
    bl_json_init(&j, buf, sizeof(buf));
    bl_json_obj(&j);
    CHECK(bl_json_end(&j) == 0);
    bl_json_init(&j, buf, sizeof(buf));
    bl_json_obj_end(&j);
    CHECK(bl_json_end(&j) == 0);
}

static void test_connect_encoding(void)
{
    uint8_t buf[256];
    MqttConnect c = { 0 };
    c.client_id = "pg-a1b2c3d4-app";
    c.username = "user";
    c.password = "pass";
    c.will_topic = "playguard/a1b2c3d4/availability";
    c.will_payload = "offline";
    c.will_len = 7;
    c.will_retain = true;
    c.keepalive_s = 30;
    c.clean_session = true;
    const size_t n = mqtt_encode_connect(buf, sizeof(buf), &c);
    CHECK(n > 0);
    CHECK(buf[0] == 0x10);
    MqttPacket p;
    CHECK(mqtt_parse(buf, n, sizeof(buf), &p) == MQTT_OK);
    CHECK(p.type == MQTT_CONNECT && p.size == n);
    const uint8_t *b = p.body;
    CHECK(b[0] == 0 && b[1] == 4 && !memcmp(b + 2, "MQTT", 4) && b[6] == 4);
    // user, password, will retain, will QoS 0, will, clean session
    CHECK(b[7] == (0x80 | 0x40 | 0x20 | 0x04 | 0x02));
    CHECK(b[8] == 0 && b[9] == 30);
    size_t at = 10;
    CHECK(b[at] == 0 && b[at + 1] == strlen(c.client_id) && !memcmp(b + at + 2, c.client_id, strlen(c.client_id)));
    at += 2 + strlen(c.client_id);
    CHECK(b[at + 1] == strlen(c.will_topic));
    at += 2 + strlen(c.will_topic);
    CHECK(b[at + 1] == 7 && !memcmp(b + at + 2, "offline", 7));
    at += 9;
    CHECK(b[at + 1] == 4 && !memcmp(b + at + 2, "user", 4));
    at += 6;
    CHECK(b[at + 1] == 4 && !memcmp(b + at + 2, "pass", 4));
    at += 6;
    CHECK(at == p.body_len);

    // No user: no password either; buffer too small: 0.
    c.username = NULL;
    const size_t n2 = mqtt_encode_connect(buf, sizeof(buf), &c);
    CHECK(n2 > 0 && (buf[9] & 0xC0) == 0);   // connect flags: no user, no password
    CHECK(mqtt_encode_connect(buf, 10, &c) == 0);
    // A will topic with a wildcard is refused.
    c.will_topic = "a/#";
    CHECK(mqtt_encode_connect(buf, sizeof(buf), &c) == 0);
}

static void test_publish_roundtrip(void)
{
    uint8_t buf[512];
    const char *payload = "{\"x\":1}";
    const size_t h = mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "playguard/id/state", strlen(payload), 0, true, false, 0);
    CHECK(h > 0);
    memcpy(buf + h, payload, strlen(payload));
    MqttPacket p;
    CHECK(mqtt_parse(buf, h + strlen(payload), sizeof(buf), &p) == MQTT_OK);
    MqttPublish m;
    CHECK(mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_OK);
    CHECK(m.retain && m.qos == 0 && !m.dup);
    CHECK(m.topic_len == strlen("playguard/id/state") && !memcmp(m.topic, "playguard/id/state", m.topic_len));
    CHECK(m.payload_len == strlen(payload) && !memcmp(m.payload, payload, m.payload_len));

    // QoS 1 carries a packet id (and refuses 0); QoS 2 is not sent.
    CHECK(mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "t", 0, 1, false, false, 0) == 0);
    const size_t h1 = mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "t", 0, 1, false, true, 0x1234);
    CHECK(h1 > 0 && (buf[0] & 0x0F) == 0x0A);
    CHECK(mqtt_parse(buf, h1, sizeof(buf), &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_OK);
    CHECK(m.qos == 1 && m.dup && m.packet_id == 0x1234 && m.payload_len == 0);
    CHECK(mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "t", 0, 2, false, false, 1) == 0);
    // Wildcards and empty topics cannot be published to.
    CHECK(mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "a/+/b", 0, 0, false, false, 0) == 0);
    CHECK(mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "a/#", 0, 0, false, false, 0) == 0);
    CHECK(mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "", 0, 0, false, false, 0) == 0);
    // An empty retained payload (clears a retained message).
    const size_t he = mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V311, "a/b/set", 0, 0, true, false, 0);
    CHECK(he == 2 + 2 + 7 && buf[1] == 9);
}

static void test_remaining_length(void)
{
    // The boundaries of the variable length: 127/128, 16383/16384, 2097151/2097152.
    static uint8_t big[2100000 + 16];
    const size_t sizes[] = { 0, 1, 127 - 4, 128 - 4, 16383 - 4, 16384 - 4, 2097151 - 4, 2097152 - 4 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const size_t h = mqtt_encode_publish_head(big, sizeof(big), MQTT_V311, "ab", sizes[i], 0, false, false, 0);
        CHECK(h > 0);
        const size_t remaining = 2 + 2 + sizes[i];
        const size_t len_bytes = remaining < 128 ? 1 : remaining < 16384 ? 2 : remaining < 2097152 ? 3 : 4;
        CHECK(h == 1 + len_bytes + 4);
        memset(big + h, 'x', sizes[i]);
        MqttPacket p;
        CHECK(mqtt_parse(big, h + sizes[i], sizeof(big), &p) == MQTT_OK);
        CHECK(p.body_len == remaining && p.size == h + sizes[i]);
        // One byte short: incomplete.
        if (h + sizes[i] > 2) CHECK(mqtt_parse(big, h + sizes[i] - 1, sizeof(big), &p) == MQTT_INCOMPLETE);
        // Over the caller's limit: too large (decided from the header alone).
        if (h + sizes[i] > 64) CHECK(mqtt_parse(big, h, 64, &p) == MQTT_TOO_LARGE);
    }
    // A fifth length byte is malformed; reserved types 0 and 15 too.
    const uint8_t five[] = { 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    MqttPacket p;
    CHECK(mqtt_parse(five, sizeof(five), 1u << 30, &p) == MQTT_MALFORMED);
    const uint8_t zero[] = { 0x00, 0x00 };
    CHECK(mqtt_parse(zero, 2, 64, &p) == MQTT_MALFORMED);
    const uint8_t fifteen[] = { 0xF0, 0x00 };
    CHECK(mqtt_parse(fifteen, 2, 64, &p) == MQTT_MALFORMED);
    // Fewer than two bytes, or a length still running: incomplete.
    CHECK(mqtt_parse(zero, 1, 64, &p) == MQTT_INCOMPLETE);
    const uint8_t running[] = { 0x30, 0x80 };
    CHECK(mqtt_parse(running, 2, 64, &p) == MQTT_INCOMPLETE);
}

static void test_other_packets(void)
{
    uint8_t buf[256];
    const char *topics[] = { "playguard/id/+/set", "homeassistant/status" };
    const uint8_t qos[] = { 1, 0 };
    const size_t n = mqtt_encode_subscribe(buf, sizeof(buf), MQTT_V311, 7, topics, qos, 2);
    CHECK(n > 0 && buf[0] == 0x82);
    MqttPacket p;
    CHECK(mqtt_parse(buf, n, sizeof(buf), &p) == MQTT_OK && p.type == MQTT_SUBSCRIBE);
    CHECK(p.body[0] == 0 && p.body[1] == 7);
    CHECK(p.body[2] == 0 && p.body[3] == strlen(topics[0]));
    CHECK(p.body[4 + strlen(topics[0])] == 1);
    CHECK(mqtt_encode_subscribe(buf, sizeof(buf), MQTT_V311, 0, topics, qos, 2) == 0);   // id 0
    CHECK(mqtt_encode_subscribe(buf, sizeof(buf), MQTT_V311, 1, topics, qos, 0) == 0);   // nothing

    const uint8_t suback[] = { 0x90, 0x04, 0x00, 0x07, 0x01, 0x80 };
    CHECK(mqtt_parse(suback, sizeof(suback), 64, &p) == MQTT_OK);
    uint16_t id = 0;
    uint8_t codes[4];
    size_t count = 0;
    CHECK(mqtt_decode_suback(&p, MQTT_V311, &id, codes, 4, &count) == MQTT_OK);
    CHECK(id == 7 && count == 2 && codes[0] == 1 && codes[1] == 0x80);
    const uint8_t bad_suback[] = { 0x90, 0x03, 0x00, 0x07, 0x03 };
    CHECK(mqtt_parse(bad_suback, sizeof(bad_suback), 64, &p) == MQTT_OK);
    CHECK(mqtt_decode_suback(&p, MQTT_V311, &id, codes, 4, &count) == MQTT_MALFORMED);

    const uint8_t connack[] = { 0x20, 0x02, 0x01, 0x05 };
    MqttConnack ca;
    CHECK(mqtt_parse(connack, 4, 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V311, &ca) == MQTT_OK);
    CHECK(ca.session_present && ca.return_code == 5);
    CHECK(!strcmp(mqtt_connack_text(5), "not authorised"));
    CHECK(!strcmp(mqtt_connack_text(4), "bad user name or password"));
    const uint8_t bad_connack[] = { 0x20, 0x02, 0x02, 0x00 };
    CHECK(mqtt_parse(bad_connack, 4, 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V311, &ca) == MQTT_MALFORMED);

    CHECK(mqtt_encode_puback(buf, sizeof(buf), 0xBEEF) == 4);
    CHECK(mqtt_parse(buf, 4, 64, &p) == MQTT_OK);
    CHECK(mqtt_decode_puback(&p, MQTT_V311, &id) == MQTT_OK && id == 0xBEEF);
    CHECK(mqtt_encode_empty(buf, sizeof(buf), MQTT_PINGREQ) == 2 && buf[0] == 0xC0 && buf[1] == 0);
    CHECK(mqtt_encode_empty(buf, sizeof(buf), MQTT_DISCONNECT) == 2 && buf[0] == 0xE0);
    CHECK(mqtt_encode_empty(buf, sizeof(buf), MQTT_PUBLISH) == 0);

    // A PUBLISH whose topic runs past the packet, or holds a wildcard / NUL.
    const uint8_t cut[] = { 0x30, 0x03, 0x00, 0x05, 'a' };
    CHECK(mqtt_parse(cut, sizeof(cut), 64, &p) == MQTT_OK);
    MqttPublish m;
    CHECK(mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_MALFORMED);
    const uint8_t wild[] = { 0x30, 0x04, 0x00, 0x02, 'a', '#' };
    CHECK(mqtt_parse(wild, sizeof(wild), 64, &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_MALFORMED);
    const uint8_t nul[] = { 0x30, 0x04, 0x00, 0x02, 'a', 0 };
    CHECK(mqtt_parse(nul, sizeof(nul), 64, &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_MALFORMED);
    // QoS 1 with packet id 0.
    const uint8_t qos1_zero[] = { 0x32, 0x05, 0x00, 0x01, 'a', 0x00, 0x00 };
    CHECK(mqtt_parse(qos1_zero, sizeof(qos1_zero), 64, &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V311, &m) == MQTT_MALFORMED);
}

static void test_mqtt5(void)
{
    uint8_t buf[512];
    MqttConnect c;
    memset(&c, 0, sizeof(c));
    c.client_id = "pg-a1b2c3d4-app";
    c.username = "user";
    c.password = "pass";
    c.will_topic = "playguard/a1b2c3d4/availability";
    c.will_payload = "offline";
    c.will_len = 7;
    c.will_retain = true;
    c.keepalive_s = 30;
    c.clean_session = true;
    c.version = MQTT_V5;
    c.max_packet = 4096;
    size_t n = mqtt_encode_connect(buf, sizeof(buf), &c);
    MqttPacket p;
    CHECK(n > 0 && mqtt_parse(buf, n, sizeof(buf), &p) == MQTT_OK && p.type == MQTT_CONNECT);
    const uint8_t *b = p.body;
    CHECK(b[6] == 5 && b[7] == (0x80 | 0x40 | 0x20 | 0x04 | 0x02) && b[8] == 0 && b[9] == 30);
    // Properties: the maximum packet size only.
    CHECK(b[10] == 5 && b[11] == 0x27 && b[12] == 0 && b[13] == 0 && b[14] == 0x10 && b[15] == 0);
    size_t at = 16;
    CHECK(b[at + 1] == strlen(c.client_id));
    at += 2 + strlen(c.client_id);
    CHECK(b[at] == 0);   // will properties: none
    at += 1;
    CHECK(b[at + 1] == strlen(c.will_topic));
    at += 2 + strlen(c.will_topic) + 2 + 7 + 6 + 6;
    CHECK(at == p.body_len);
    // No maximum packet size: an empty property list.
    c.max_packet = 0;
    n = mqtt_encode_connect(buf, sizeof(buf), &c);
    CHECK(mqtt_parse(buf, n, sizeof(buf), &p) == MQTT_OK && p.body[10] == 0);

    // PUBLISH and SUBSCRIBE carry an empty property list.
    n = mqtt_encode_publish_head(buf, sizeof(buf), MQTT_V5, "a/b", 2, 0, true, false, 0);
    CHECK(n == 2 + 2 + 3 + 1 && buf[1] == 2 + 3 + 1 + 2 && buf[n - 1] == 0);
    memcpy(buf + n, "on", 2);
    MqttPublish m;
    CHECK(mqtt_parse(buf, n + 2, sizeof(buf), &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V5, &m) == MQTT_OK);
    CHECK(m.topic_len == 3 && m.payload_len == 2 && !memcmp(m.payload, "on", 2) && m.retain);
    const char *topics[] = { "playguard/id/+/set" };
    const uint8_t qos[] = { 1 };
    n = mqtt_encode_subscribe(buf, sizeof(buf), MQTT_V5, 9, topics, qos, 1);
    CHECK(n > 0 && mqtt_parse(buf, n, sizeof(buf), &p) == MQTT_OK);
    CHECK(p.body[2] == 0 && p.body[4] == strlen(topics[0]) && p.body[p.body_len - 1] == 1);

    // A PUBLISH from the broker with properties (message expiry, a user
    // property): skipped, the payload found.
    const uint8_t pub[] = { 0x31, 0x16, 0x00, 0x01, 't', 0x0D, 0x02, 0x00, 0x00, 0x00, 0x3C,
                            0x26, 0x00, 0x01, 'k', 0x00, 0x02, 'v', 'v', 'O', 'F', 'F', '!', '!' };
    CHECK(mqtt_parse(pub, sizeof(pub), 64, &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V5, &m) == MQTT_OK);
    CHECK(m.retain && m.payload_len == 5 && !memcmp(m.payload, "OFF!!", 5));
    // Property lengths that run past the packet, or an unknown property.
    const uint8_t pub_cut[] = { 0x30, 0x06, 0x00, 0x01, 't', 0x09, 0x02, 0x00 };
    CHECK(mqtt_parse(pub_cut, sizeof(pub_cut), 64, &p) == MQTT_OK && mqtt_decode_publish(&p, MQTT_V5, &m) == MQTT_MALFORMED);
    const uint8_t pub_unknown[] = { 0x30, 0x06, 0x00, 0x01, 't', 0x02, 0x7E, 0x00 };
    CHECK(mqtt_parse(pub_unknown, sizeof(pub_unknown), 64, &p) == MQTT_OK &&
           mqtt_decode_publish(&p, MQTT_V5, &m) == MQTT_MALFORMED);

    // CONNACK: accepted, with a server keep-alive, a maximum packet size and
    // retained messages available.
    const uint8_t ack[] = { 0x20, 0x0D, 0x00, 0x00, 0x0A, 0x13, 0x00, 0x3C, 0x27, 0x00, 0x01, 0x00, 0x00, 0x25, 0x01 };
    MqttConnack ca;
    CHECK(mqtt_parse(ack, sizeof(ack), 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V5, &ca) == MQTT_OK);
    CHECK(ca.return_code == 0 && !ca.v311 && ca.has_server_keepalive && ca.server_keepalive == 60);
    CHECK(ca.max_packet == 65536 && !ca.retain_unavailable && !ca.wildcard_unavailable && !ca.reason[0]);
    // Refused, with a reason string; retained messages and wildcards unavailable.
    const uint8_t refused[] = { 0x20, 0x14, 0x00, 0x87, 0x11, 0x25, 0x00, 0x28, 0x00, 0x1F, 0x00, 0x0A,
                                'n', 'o', ' ', 'a', 'c', 'c', 'e', 's', 's', '\n' };
    CHECK(mqtt_parse(refused, sizeof(refused), 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V5, &ca) == MQTT_OK);
    CHECK(ca.return_code == 0x87 && ca.retain_unavailable && ca.wildcard_unavailable);
    CHECK(!strcmp(ca.reason, "no access "));   // control characters blanked
    CHECK(!strcmp(mqtt_reason_text(0x87), "not authorised"));
    CHECK(!strcmp(mqtt_reason_text(MQTT_RC_UNSUPPORTED_VERSION), "the broker does not speak this MQTT version"));
    // A 3.1.1 broker's answer to a 5.0 CONNECT.
    const uint8_t old[] = { 0x20, 0x02, 0x00, 0x01 };
    CHECK(mqtt_parse(old, sizeof(old), 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V5, &ca) == MQTT_OK);
    CHECK(ca.v311 && ca.return_code == 1);
    // A property list longer than the packet.
    const uint8_t ack_cut[] = { 0x20, 0x04, 0x00, 0x00, 0x05, 0x13 };
    CHECK(mqtt_parse(ack_cut, sizeof(ack_cut), 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V5, &ca) == MQTT_MALFORMED);
    // 3.1.1 never has properties.
    CHECK(mqtt_parse(ack, sizeof(ack), 64, &p) == MQTT_OK && mqtt_decode_connack(&p, MQTT_V311, &ca) == MQTT_MALFORMED);

    // SUBACK: properties, then a reason code per topic (0x87: not authorised).
    const uint8_t suback[] = { 0x90, 0x05, 0x00, 0x09, 0x00, 0x01, 0x87 };
    uint16_t id = 0;
    uint8_t codes[4];
    size_t count = 0;
    CHECK(mqtt_parse(suback, sizeof(suback), 64, &p) == MQTT_OK);
    CHECK(mqtt_decode_suback(&p, MQTT_V5, &id, codes, 4, &count) == MQTT_OK);
    CHECK(id == 9 && count == 2 && codes[0] == 1 && codes[1] == 0x87);
    CHECK(mqtt_decode_suback(&p, MQTT_V311, &id, codes, 4, &count) == MQTT_MALFORMED);   // 0x87 is 5.0's
    // PUBACK with a reason code and no properties.
    const uint8_t puback[] = { 0x40, 0x03, 0x12, 0x34, 0x10 };
    CHECK(mqtt_parse(puback, sizeof(puback), 64, &p) == MQTT_OK && mqtt_decode_puback(&p, MQTT_V5, &id) == MQTT_OK);
    CHECK(id == 0x1234 && mqtt_decode_puback(&p, MQTT_V311, &id) == MQTT_MALFORMED);

    // DISCONNECT from the broker: a reason code and a reason string.
    const uint8_t disc[] = { 0xE0, 0x0A, 0x8E, 0x08, 0x1F, 0x00, 0x05, 't', 'a', 'k', 'e', 'n' };
    uint8_t code = 0;
    char reason[32];
    CHECK(mqtt_parse(disc, sizeof(disc), 64, &p) == MQTT_OK);
    CHECK(mqtt_decode_disconnect(&p, &code, reason, sizeof(reason)) == MQTT_OK);
    CHECK(code == 0x8E && !strcmp(reason, "taken") && !strcmp(mqtt_reason_text(code), "session taken over by another client"));
    const uint8_t disc_plain[] = { 0xE0, 0x00 };
    CHECK(mqtt_parse(disc_plain, sizeof(disc_plain), 64, &p) == MQTT_OK);
    CHECK(mqtt_decode_disconnect(&p, &code, reason, sizeof(reason)) == MQTT_OK && code == 0 && !reason[0]);
}

static void test_topic_matching(void)
{
    CHECK(mqtt_topic_matches("a/+/set", "a/limit_mon/set", 15));
    CHECK(!mqtt_topic_matches("a/+/set", "a/x/y/set", 9));
    CHECK(mqtt_topic_matches("a/#", "a", 1));
    CHECK(mqtt_topic_matches("a/#", "a/b/c", 5));
    CHECK(mqtt_topic_matches("#", "x/y", 3));
    CHECK(!mqtt_topic_matches("a/b", "a/bc", 4));
    CHECK(mqtt_topic_matches("a/+", "a/", 2));
    CHECK(!mqtt_topic_matches("a/+", "a", 1));
    CHECK(mqtt_topic_valid("a/b") && !mqtt_topic_valid("a/+") && !mqtt_topic_valid(""));
}

// ---------------------------------------------------------------- key=value

typedef struct {
    char   keys[8][48];
    char   values[8][512];
    size_t n;
} Seen;

static void collect(void *ctx, const char *key, const char *value)
{
    Seen *s = (Seen *)ctx;
    REQUIRE(s->n < 8);
    snprintf(s->keys[s->n], sizeof(s->keys[s->n]), "%s", key);
    snprintf(s->values[s->n], sizeof(s->values[s->n]), "%s", value);
    s->n++;
}

static void test_kv(void)
{
    const char *text = "\xEF\xBB\xBF# comment\r\n"
                       "enabled=1\r\n"
                       "  host = broker.lan\n"      // spaces around the key, not the value
                       "password= p=a ss#word\n"    // kept verbatim after '='
                       "\t# indented comment\n"
                       "noequals\n"
                       "=novalue\n"
                       "empty=\n"
                       "last=no newline";
    Seen s;
    memset(&s, 0, sizeof(s));
    bl_kv_each(text, strlen(text), collect, &s);
    CHECK(s.n == 5);
    CHECK(!strcmp(s.keys[0], "enabled") && !strcmp(s.values[0], "1"));
    CHECK(!strcmp(s.keys[1], "host") && !strcmp(s.values[1], " broker.lan"));
    CHECK(!strcmp(s.keys[2], "password") && !strcmp(s.values[2], " p=a ss#word"));
    CHECK(!strcmp(s.keys[3], "empty") && !strcmp(s.values[3], ""));
    CHECK(!strcmp(s.keys[4], "last") && !strcmp(s.values[4], "no newline"));

    memset(&s, 0, sizeof(s));
    CHECK(bl_kv_each(text, strlen(text), collect, &s) == 0);

    // A key or a value too long for the reader skips its line, counted.
    char longer[700];
    memset(longer, 'v', sizeof(longer));
    memcpy(longer, "k=", 2);
    memcpy(longer + 600, "\nok=1\n", 7);
    memset(&s, 0, sizeof(s));
    CHECK(bl_kv_each(longer, strlen(longer), collect, &s) == 1);
    CHECK(s.n == 1 && !strcmp(s.keys[0], "ok"));
    char long_key[80];
    memset(long_key, 'k', BL_KV_KEY_MAX);
    snprintf(long_key + BL_KV_KEY_MAX, sizeof(long_key) - BL_KV_KEY_MAX, "=1\nok=1\n");
    memset(&s, 0, sizeof(s));
    CHECK(bl_kv_each(long_key, strlen(long_key), collect, &s) == 1);
    CHECK(s.n == 1 && !strcmp(s.keys[0], "ok"));

    uint32_t u = 7;
    CHECK(bl_parse_uint("65535", 1, 65535, &u) && u == 65535);
    CHECK(!bl_parse_uint("65536", 1, 65535, &u) && u == 65535);
    CHECK(!bl_parse_uint("", 0, 10, &u) && !bl_parse_uint(NULL, 0, 10, &u) && !bl_parse_uint("1a", 0, 10, &u));
    CHECK(!bl_parse_uint("-1", 0, 10, &u) && !bl_parse_uint("99999999999", 0, 0xFFFFFFFFu, &u));
    bool b = false;
    CHECK(bl_parse_bool("Yes", &b) && b && bl_parse_bool("OFF", &b) && !b);
    CHECK(bl_parse_bool("1", &b) && b && bl_parse_bool("false", &b) && !b);
    CHECK(!bl_parse_bool("maybe", &b) && !b);
}

// ------------------------------------------------- key=value, round trip

// A homebrew's file of schema 1: two keys it knows, the others kept.
typedef struct {
    BlKvFile *f;
    char      volume[8];
    char      theme[16];
} Prefs;

static void on_prefs(void *ctx, const char *key, const char *value)
{
    Prefs *p = (Prefs *)ctx;
    if (!strcmp(key, "volume")) snprintf(p->volume, sizeof(p->volume), "%s", value);
    else if (!strcmp(key, "theme")) snprintf(p->theme, sizeof(p->theme), "%s", value);
    else bl_kv_keep(p->f, key, value);
}

static size_t write_prefs(const BlKvFile *f, const Prefs *p, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "schema=1\nvolume=%s\ntheme=%s\n", p->volume, p->theme);
    if (n < 0 || (size_t)n >= cap) return 0;
    return bl_kv_write_kept(f, out, cap, (size_t)n);
}

static void test_kv_schema(void)
{
    char kept[64], out[256];
    BlKvFile f;
    Prefs p;

    // Absent: 1, and the schema line is not handed over
    CHECK(bl_kv_schema("volume=3\n", 9) == BL_KV_SCHEMA_ABSENT);
    CHECK(bl_kv_schema("", 0) == 1);
    CHECK(bl_kv_schema("schema= 2 \n", 11) == 2);
    CHECK(bl_kv_schema("schema=1\nschema=4\n", 18) == 4);
    static const char *const bad[] = { "two", "0", "-1", "1.5", "", "99999999999", "2 3" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char line[40];
        const int n = snprintf(line, sizeof(line), "schema=%s\n", bad[i]);
        CHECK(bl_kv_schema(line, (size_t)n) == 0);
    }

    // Unknown keys come back after the caller's own, as read
    const char *mine = "# Prefs\nvolume=3\nsparkle= on\ntheme=moss\ncolour=#3ddc97\n";
    memset(&p, 0, sizeof(p));
    p.f = &f;
    bl_kv_file_init(&f, kept, sizeof(kept));
    bl_kv_read(&f, mine, strlen(mine), on_prefs, &p);
    CHECK(f.schema == 1 && f.skipped == 0 && !f.overflow);
    CHECK(!strcmp(p.volume, "3") && !strcmp(p.theme, "moss"));
    CHECK(!strcmp(kept, "sparkle= on\ncolour=#3ddc97\n"));
    CHECK(bl_kv_may_rewrite(&f, 1));
    snprintf(p.volume, sizeof(p.volume), "5");
    size_t n = write_prefs(&f, &p, out, sizeof(out));
    CHECK(n == strlen(out) && !strcmp(out, "schema=1\nvolume=5\ntheme=moss\nsparkle= on\ncolour=#3ddc97\n"));
    // Read again, it gives the same (the schema line is the writer's, once)
    Prefs again;
    memset(&again, 0, sizeof(again));
    again.f = &f;
    bl_kv_read(&f, out, n, on_prefs, &again);
    CHECK(!strcmp(again.volume, "5") && !strcmp(kept, "sparkle= on\ncolour=#3ddc97\n"));
    CHECK(write_prefs(&f, &again, out, sizeof(out)) == n);
    // Too small for what is kept: nothing written
    CHECK(write_prefs(&f, &again, out, n) == 0);

    // Newer: read for what is known, never written over
    const char *newer = "schema=2\nvolume=4\nshape=round\n";
    memset(&p, 0, sizeof(p));
    p.f = &f;
    bl_kv_read(&f, newer, strlen(newer), on_prefs, &p);
    CHECK(f.schema == 2 && !strcmp(p.volume, "4"));
    CHECK(!bl_kv_may_rewrite(&f, 1) && bl_kv_may_rewrite(&f, 2));
    bl_kv_read(&f, "schema=soon\n", 12, on_prefs, &p);
    CHECK(f.schema == 0 && !bl_kv_may_rewrite(&f, 1) && !bl_kv_may_rewrite(&f, 1000));
    bl_kv_read(&f, "volume=4\n", 9, on_prefs, &p);
    CHECK(f.schema == 1 && bl_kv_may_rewrite(&f, 1));

    // A line skipped, or one to keep that does not fit: not written over
    char longer[700];
    memset(longer, 'v', sizeof(longer));
    memcpy(longer, "note=", 5);
    memcpy(longer + 600, "\nvolume=2\n", 11);
    bl_kv_read(&f, longer, strlen(longer), on_prefs, &p);
    CHECK(f.skipped == 1 && !strcmp(p.volume, "2") && !bl_kv_may_rewrite(&f, 1));
    const char *many = "a=0123456789012345678901234567890123456789\nb=0123456789012345678901234567890123456789\n";
    bl_kv_read(&f, many, strlen(many), on_prefs, &p);
    CHECK(f.skipped == 0 && f.overflow && !bl_kv_may_rewrite(&f, 1));
    CHECK(!strncmp(kept, "a=", 2) && f.len == strlen(kept));
}

int main(void)
{
    test_json();
    test_connect_encoding();
    test_publish_roundtrip();
    test_remaining_length();
    test_other_packets();
    test_mqtt5();
    test_topic_matching();
    test_kv();
    test_kv_schema();
    return CHECK_DONE("codec: all tests passed");
}
