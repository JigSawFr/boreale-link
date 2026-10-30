// mqtt_client — a small MQTT 5.0 / 3.1.1 client over any byte stream (BlIo):
// plain TCP (bl_net.c), TLS through the console's ssl service
// (bl_tls.c), or a scripted broker in the host tests. One thread
// drives it; nothing is allocated (the receive buffer is the caller's).
//
// Outbound publishes are QoS 0: the retained state heals itself at the next
// publish, so there is nothing to resend. Inbound messages may be QoS 1 (the
// subscription asks for it): each one is acknowledged after the callback.
// The version is the CONNECT's (MqttConnect.version); in 5.0 the client
// follows the broker's keep-alive and maximum packet size, and words its
// refusals with the reason codes and strings it sends.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "mqtt_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

// A connection. Every call returns 0 or a negative value on failure, with an
// English reason in `err` when the call takes one.
typedef struct {
    void *ctx;
    int  (*open)(void *ctx, const char *host, uint16_t port, int timeout_ms, char *err, size_t err_size);
    // Writes all `len` bytes within `timeout_ms`.
    int  (*write)(void *ctx, const void *buf, size_t len, int timeout_ms);
    // Up to `len` bytes: > 0 read, 0 nothing within `timeout_ms`, < 0 closed or failed.
    long (*read)(void *ctx, void *buf, size_t len, int timeout_ms);
    void (*close)(void *ctx);
} BlIo;

typedef void (*MqttOnMessage)(void *user, const MqttPublish *msg);

typedef struct {
    BlIo          io;
    uint8_t      *rx;
    size_t        rx_cap, rx_len;
    size_t        skip;             // bytes of an oversized packet still to drop
    uint8_t       tx[512];          // fixed headers and small packets
    bool          open;             // the stream is open
    bool          connected;        // CONNACK accepted
    uint16_t      keepalive_s;
    uint64_t      last_out_ms;      // last packet sent
    uint64_t      last_in_ms;       // last bytes received
    uint64_t      ping_sent_ms;     // PINGREQ waiting for its PINGRESP (0: none)
    uint16_t      next_id;
    MqttOnMessage on_message;
    void         *user;
    uint64_t    (*now_ms)(void);
    uint32_t      dropped;          // oversized messages dropped
    uint8_t       version;          // MQTT_V5 or MQTT_V311, as connected
    uint32_t      max_out;          // 5.0: the largest packet the broker takes (0: no limit)
    uint32_t      too_large;        // publishes not sent: larger than max_out
    bool          version_refused;  // the last CONNECT was refused for its version, or the broker
                                    // closed the stream without answering it
    char          error[128];
} MqttClient;

void mqtt_client_init(MqttClient *c, BlIo io, uint8_t *rx, size_t rx_cap, uint64_t (*now_ms)(void),
                      MqttOnMessage on_message, void *user);

// Opens the stream, sends CONNECT and waits for an accepted CONNACK.
int  mqtt_client_connect(MqttClient *c, const char *host, uint16_t port, const MqttConnect *opts, int timeout_ms);
// Subscribes and waits for the SUBACK; messages delivered meanwhile (retained
// ones arrive right away) go to the callback. Fails when one of the first
// `required` topics is refused; the others are optional: how many of them the
// broker refused is returned (0: every topic granted).
int  mqtt_client_subscribe(MqttClient *c, const char *const *topics, const uint8_t *qos, size_t count,
                           size_t required, int timeout_ms);
// QoS 0.
int  mqtt_client_publish(MqttClient *c, const char *topic, const void *payload, size_t len, bool retain);
// The same with its payload in pieces (a file read a piece at a time): the
// header says `len`, then exactly `len` bytes follow, through _more. A
// payload that cannot be finished leaves the stream unusable: close it.
int  mqtt_client_publish_begin(MqttClient *c, const char *topic, size_t len, bool retain);
int  mqtt_client_publish_more(MqttClient *c, const void *bytes, size_t n);
// Reads for up to `timeout_ms`, delivers messages, keeps the connection alive
// (PINGREQ, and a missing PINGRESP closes it).
int  mqtt_client_poll(MqttClient *c, int timeout_ms);
// DISCONNECT (when asked and connected), then closes the stream. The broker
// does not send the last will after a DISCONNECT.
void mqtt_client_close(MqttClient *c, bool send_disconnect);

#ifdef __cplusplus
}
#endif
