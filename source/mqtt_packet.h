// mqtt_packet — MQTT 5.0 and 3.1.1 packets, encoded into and decoded from
// caller buffers. Only what a small client needs: CONNECT (with a last will
// and a user name / password), CONNACK, PUBLISH (QoS 0 and 1), PUBACK,
// SUBSCRIBE, SUBACK, PINGREQ, PINGRESP, DISCONNECT. In 5.0 the client sends
// no property but its maximum packet size, and reads the few the broker's
// CONNACK and DISCONNECT carry that change what it may do (server keep-alive,
// maximum packet size, retained messages and wildcards available, the reason
// string); the others are skipped. No allocation, no I/O: the host tests feed
// it bytes (tests/codec), mqtt_client.c does the sockets.
// References: OASIS MQTT Version 3.1.1 (2014), MQTT Version 5.0 (2019).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTT_CONNECT     = 1,
    MQTT_CONNACK     = 2,
    MQTT_PUBLISH     = 3,
    MQTT_PUBACK      = 4,
    MQTT_SUBSCRIBE   = 8,
    MQTT_SUBACK      = 9,
    MQTT_UNSUBSCRIBE = 10,
    MQTT_UNSUBACK    = 11,
    MQTT_PINGREQ     = 12,
    MQTT_PINGRESP    = 13,
    MQTT_DISCONNECT  = 14,
} MqttType;

typedef enum {
    MQTT_OK = 0,
    MQTT_INCOMPLETE,    // more bytes needed
    MQTT_MALFORMED,     // not a valid packet: the connection must be closed
    MQTT_TOO_LARGE,     // longer than the limit the caller set
} MqttResult;

// The largest "remaining length" MQTT allows (4 length bytes).
#define MQTT_MAX_REMAINING 268435455u

// Protocol levels.
#define MQTT_V311 4
#define MQTT_V5   5

// 5.0 reason codes the client acts on (the rest are worded by mqtt_reason_text).
#define MQTT_RC_UNSUPPORTED_VERSION 0x84

typedef struct {
    const char *client_id;      // 1..23 characters [0-9a-zA-Z-] is what every broker accepts
    const char *username;       // NULL: none
    const char *password;       // NULL: none (only sent with a username)
    const char *will_topic;     // NULL: no last will
    const void *will_payload;
    size_t      will_len;
    bool        will_retain;
    uint8_t     will_qos;       // 0 or 1
    uint16_t    keepalive_s;
    bool        clean_session;
    uint8_t     version;        // MQTT_V5, else 3.1.1
    uint32_t    max_packet;     // 5.0: the largest packet the client takes (0: not said)
} MqttConnect;

// Each encoder returns the bytes written, or 0 when `cap` is too small or an
// argument is invalid (a topic with a wildcard in a PUBLISH, QoS above 1 …).
size_t mqtt_encode_connect(uint8_t *buf, size_t cap, const MqttConnect *c);
// The fixed header, the topic and the packet id of a PUBLISH whose payload,
// `payload_len` bytes, is sent right after it (so a large payload is not
// copied). `packet_id` is only written for QoS 1; in 5.0 an empty property
// list follows.
size_t mqtt_encode_publish_head(uint8_t *buf, size_t cap, uint8_t version, const char *topic, size_t payload_len,
                                uint8_t qos, bool retain, bool dup, uint16_t packet_id);
// The same in both versions (5.0's reason code "success" is implied).
size_t mqtt_encode_puback(uint8_t *buf, size_t cap, uint16_t packet_id);
size_t mqtt_encode_subscribe(uint8_t *buf, size_t cap, uint8_t version, uint16_t packet_id,
                             const char *const *topics, const uint8_t *qos, size_t count);
// PINGREQ or DISCONNECT (2 bytes; a 5.0 DISCONNECT with no reason code is a
// normal disconnection, which keeps the last will unsent).
size_t mqtt_encode_empty(uint8_t *buf, size_t cap, MqttType type);

// One packet at the start of `buf` (`len` bytes received so far).
typedef struct {
    uint8_t        type;       // MqttType
    uint8_t        flags;      // low nibble of the first byte
    const uint8_t *body;       // variable header + payload, inside `buf`
    size_t         body_len;
    size_t         size;       // whole packet, header included
} MqttPacket;

// MQTT_INCOMPLETE until the whole packet is there. `max_size` bounds the whole
// packet (MQTT_TOO_LARGE beyond: the caller cannot hold it).
MqttResult mqtt_parse(const uint8_t *buf, size_t len, size_t max_size, MqttPacket *out);

typedef struct {
    bool     session_present;
    uint8_t  return_code;     // 0 accepted; 3.1.1: 1..5 (mqtt_connack_text); 5.0: a reason code (mqtt_reason_text)
    bool     v311;            // a 3.1.1 answer (also what a 3.1.1 broker sends to a 5.0 CONNECT)
    // 5.0 properties
    uint16_t server_keepalive;   // 0: the client's stands
    bool     has_server_keepalive;
    uint32_t max_packet;         // the largest packet the broker takes (0: no limit said)
    bool     retain_unavailable;
    bool     wildcard_unavailable;
    char     reason[96];         // the broker's reason string ("" none)
} MqttConnack;

typedef struct {
    const char    *topic;        // NOT NUL-terminated
    size_t         topic_len;
    const uint8_t *payload;
    size_t         payload_len;
    uint8_t        qos;
    bool           retain;       // set by the broker on a retained message delivered at subscription
    bool           dup;
    uint16_t       packet_id;    // QoS 1 only
} MqttPublish;

// `version`: what the client sent in its CONNECT.
MqttResult mqtt_decode_connack(const MqttPacket *p, uint8_t version, MqttConnack *out);
MqttResult mqtt_decode_publish(const MqttPacket *p, uint8_t version, MqttPublish *out);
MqttResult mqtt_decode_puback(const MqttPacket *p, uint8_t version, uint16_t *packet_id);
// `codes` gets one code per topic (0..2 granted QoS, 0x80 and above refused).
MqttResult mqtt_decode_suback(const MqttPacket *p, uint8_t version, uint16_t *packet_id, uint8_t *codes, size_t max,
                              size_t *count);
// A DISCONNECT the broker sends (5.0 only): its reason code and string.
MqttResult mqtt_decode_disconnect(const MqttPacket *p, uint8_t *reason_code, char *reason, size_t reason_cap);

// English reason for a 3.1.1 CONNACK return code ("bad user name or password" …).
const char *mqtt_connack_text(uint8_t return_code);
// English reason for a 5.0 reason code ("not authorized", "quota exceeded" …).
const char *mqtt_reason_text(uint8_t code);

// A topic a client may publish to: not empty, at most 65535 bytes, no '+',
// '#' or NUL.
bool mqtt_topic_valid(const char *topic);
// Whether `topic` (`len` bytes) matches `filter` (with '+' and '#').
bool mqtt_topic_matches(const char *filter, const char *topic, size_t len);

#ifdef __cplusplus
}
#endif
