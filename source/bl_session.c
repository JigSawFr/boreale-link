// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_session.h"

#include <stdio.h>
#include <string.h>

static void logf_(BlSession *s, const char *fmt, const char *a, const char *b)
{
    if (!s->host.log) return;
    snprintf(s->line, sizeof(s->line), fmt, a ? a : "", b ? b : "");
    s->host.log(s->host.ctx, s->line);
}

void bl_session_init(BlSession *s, const BlSessionConf *conf, BlIo io, const BlSessionHost *host,
                     uint64_t (*now_ms)(void), MqttOnMessage on_message, void *user)
{
    memset(s, 0, sizeof(*s));
    s->conf = *conf;
    s->host = *host;
    s->now_ms = now_ms;
    mqtt_client_init(&s->mqtt, io, s->rx, sizeof(s->rx), now_ms, on_message, user);
}

void bl_session_configure(BlSession *s, const BlSessionConf *conf, bool forget_version)
{
    s->conf = *conf;
    if (forget_version) s->proto_ok = 0;   // another broker, or another version asked: try 5.0 again
}

bool bl_session_online(const BlSession *s)
{
    return s->mqtt.connected;
}

void bl_session_lost(BlSession *s)
{
    snprintf(s->status.error, sizeof(s->status.error), "%s", s->mqtt.error[0] ? s->mqtt.error : "connection lost");
    s->status.state = BlLink_Waiting;
    s->status.next_attempt_ms = s->now_ms() + s->backoff_ms;
    if (s->host.lost) s->host.lost(s->host.ctx);
    logf_(s, "remote link: %s%s", s->status.error, NULL);
}

// The console slept: the wall clock moved on further than the broker keeps a
// silent connection (one keep-alive and a half), so the broker has dropped
// this one and said "offline" for it, while the socket may still look open
// and take what is written, for nowhere. Closed without a word, at once:
// the next turn connects again. Every call that writes asks first (a clock
// set forward by as much costs one connection more, nothing else).
bool bl_session_woke(BlSession *s)
{
    const uint64_t t = s->host.now_posix ? s->host.now_posix(s->host.ctx) : 0;
    const uint64_t last = s->seen_posix;
    if (t) s->seen_posix = t;
    if (!t || !last || t <= last || !s->mqtt.connected) return false;
    const uint64_t keepalive = s->mqtt.keepalive_s ? s->mqtt.keepalive_s : BL_KEEPALIVE_S;
    if (t - last <= keepalive * 3 / 2) return false;
    mqtt_client_close(&s->mqtt, false);
    snprintf(s->mqtt.error, sizeof(s->mqtt.error), "the console slept, connecting again");
    s->backoff_ms = 0;
    bl_session_lost(s);
    return true;
}

int bl_session_publish(BlSession *s, const char *sub, const void *payload, size_t len, bool retained)
{
    bl_session_woke(s);
    if (!s->mqtt.connected) return -1;
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s", s->conf.base, sub);
    const int rc = mqtt_client_publish(&s->mqtt, topic, payload, len, retained);
    if (rc == 0) {
        s->status.publishes++;
        s->status.last_publish_ms = s->now_ms();
    }
    return rc;
}

int bl_session_publish_stream(BlSession *s, const char *sub, size_t len, bool retained, BlReadFn read, void *ctx,
                              void *buf, size_t cap)
{
    bl_session_woke(s);
    if (!s->mqtt.connected) return -1;
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s", s->conf.base, sub);
    if (mqtt_client_publish_begin(&s->mqtt, topic, len, retained) < 0) {
        if (!s->mqtt.connected) bl_session_lost(s);
        return -1;
    }
    for (size_t sent = 0; sent < len;) {
        const size_t want = len - sent < cap ? len - sent : cap;
        const size_t got = read(ctx, buf, want);
        if (got == 0 || got > want || mqtt_client_publish_more(&s->mqtt, buf, got) < 0) {
            // Half a packet went out: the broker would read what comes next
            // as its end. Closed, connected again at the next turn.
            if (s->mqtt.connected) {
                mqtt_client_close(&s->mqtt, false);
                snprintf(s->mqtt.error, sizeof(s->mqtt.error), "%s cut short, connecting again", sub);
            }
            s->backoff_ms = 0;
            bl_session_lost(s);
            return -1;
        }
        sent += got;
    }
    s->status.publishes++;
    s->status.last_publish_ms = s->now_ms();
    return 0;
}

int bl_session_publish_topic(BlSession *s, const char *topic, const void *payload, size_t len, bool retained)
{
    return mqtt_client_publish(&s->mqtt, topic, payload, len, retained);
}

int bl_session_announce(BlSession *s)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/availability", s->conf.base);
    return mqtt_client_publish(&s->mqtt, topic, "online", 6, true);
}

int bl_session_subscribe(BlSession *s, const char *const *filters, const uint8_t *qos, size_t count, size_t required)
{
    return mqtt_client_subscribe(&s->mqtt, filters, qos, count, required, BL_CONNECT_TIMEOUT_MS);
}

void bl_session_stop(BlSession *s)
{
    bl_session_woke(s);
    if (s->mqtt.connected) {
        char topic[160];
        snprintf(topic, sizeof(topic), "%s/availability", s->conf.base);
        mqtt_client_publish(&s->mqtt, topic, "offline", 7, true);
    }
    mqtt_client_close(&s->mqtt, true);
    s->status.state = BlLink_Off;
}

void bl_session_retry_now(BlSession *s)
{
    s->backoff_ms = 0;
    s->status.next_attempt_ms = 0;
    if (s->status.state != BlLink_Online) s->status.state = BlLink_Waiting;
}

static int connect_now(BlSession *s)
{
    s->status.state = BlLink_Connecting;
    char will[160];
    snprintf(will, sizeof(will), "%s/availability", s->conf.base);
    MqttConnect c;
    memset(&c, 0, sizeof(c));
    c.client_id = s->conf.client_id;
    const bool user = s->conf.username && s->conf.username[0];
    c.username = user ? s->conf.username : NULL;
    c.password = user && s->conf.password && s->conf.password[0] ? s->conf.password : NULL;
    c.will_topic = will;
    c.will_payload = "offline";
    c.will_len = 7;
    c.will_retain = true;
    c.keepalive_s = BL_KEEPALIVE_S;
    c.clean_session = true;
    // Version 0: 5.0, else 3.1.1 when the broker refuses 5.0 (or the other
    // way round once 3.1.1 worked), and the one that worked from then on.
    uint8_t other = 0;
    if (s->conf.version) {
        c.version = s->conf.version;
    } else {
        c.version = s->proto_ok ? s->proto_ok : MQTT_V5;
        other = c.version == MQTT_V5 ? MQTT_V311 : MQTT_V5;
    }
    int rc = mqtt_client_connect(&s->mqtt, s->conf.host, s->conf.port, &c, BL_CONNECT_TIMEOUT_MS);
    if (rc < 0 && other && s->mqtt.version_refused) {
        logf_(s, "remote link: MQTT %s refused (%s), trying the other version", c.version == MQTT_V5 ? "5.0" : "3.1.1",
              s->mqtt.error);
        c.version = other;
        rc = mqtt_client_connect(&s->mqtt, s->conf.host, s->conf.port, &c, BL_CONNECT_TIMEOUT_MS);
    }
    if (rc < 0) return -1;
    if (other) s->proto_ok = s->mqtt.version;
    s->status.protocol = s->mqtt.version;
    if (mqtt_client_publish(&s->mqtt, will, "online", 6, true) < 0) return -1;
    if (s->host.subscribe && s->host.subscribe(s->host.ctx) < 0) {
        // The caller gave up on a connection still open: closed, its last
        // will says offline.
        if (s->mqtt.connected) {
            mqtt_client_close(&s->mqtt, false);
            snprintf(s->mqtt.error, sizeof(s->mqtt.error), "the subscriptions failed");
        }
        return -1;
    }
    return 0;
}

int bl_session_connect_due(BlSession *s, int max_wait_ms, bool *connected)
{
    *connected = false;
    if (s->mqtt.connected) return -1;
    const uint64_t now = s->now_ms();
    if (now < s->status.next_attempt_ms) {
        s->status.state = BlLink_Waiting;
        const uint64_t left = s->status.next_attempt_ms - now;
        return (int)(left < (uint64_t)max_wait_ms ? left : (uint64_t)max_wait_ms);
    }
    s->status.connects++;
    if (connect_now(s) < 0) {
        s->backoff_ms = s->backoff_ms ? s->backoff_ms * 2 : BL_BACKOFF_MIN_MS;
        if (s->backoff_ms > BL_BACKOFF_MAX_MS) s->backoff_ms = BL_BACKOFF_MAX_MS;
        bl_session_lost(s);
        return 0;
    }
    s->backoff_ms = 0;
    s->status.state = BlLink_Online;
    s->status.error[0] = '\0';
    s->status.online_since_ms = s->now_ms();
    *connected = true;
    return -1;
}

bool bl_session_check(BlSession *s)
{
    if (s->mqtt.connected) return true;
    if (!s->backoff_ms) s->backoff_ms = BL_BACKOFF_MIN_MS;
    bl_session_lost(s);
    return false;
}

void bl_session_poll(BlSession *s, int wait_ms)
{
    if (mqtt_client_poll(&s->mqtt, wait_ms) < 0) {
        s->backoff_ms = BL_BACKOFF_MIN_MS;
        bl_session_lost(s);
    }
}
