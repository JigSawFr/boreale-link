// bl_session — one session with the broker, whatever is carried on it:
// connects with the last will "offline" on `<base>/availability` and says
// "online" there once connected, tries MQTT 3.1.1 when the broker refuses 5.0
// (or the other way round once 3.1.1 worked), waits longer after each failure
// (5 s, doubling up to 5 minutes), closes a connection the console slept
// through, and publishes under `<base>`. What goes on it is the caller's: it
// subscribes when the session tells it the connection is up, and reads the
// messages the MQTT client hands it. Single-threaded and allocation-free:
// the caller owns one BlSession and drives it from its own loop.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_RX_SIZE            4096
#define BL_KEEPALIVE_S        30
#define BL_CONNECT_TIMEOUT_MS 8000
#define BL_BACKOFF_MIN_MS     5000ULL
#define BL_BACKOFF_MAX_MS     300000ULL

typedef enum {
    BlLink_Off = 0,     // stopped
    BlLink_Waiting,     // waiting before the next attempt
    BlLink_Connecting,
    BlLink_Online,
} BlLinkState;

typedef struct {
    BlLinkState state;
    uint8_t     protocol;          // MQTT_V5 or MQTT_V311 once connected (the last one), 0 never
    char        error[128];        // the last failure ("" none)
    uint64_t    online_since_ms;
    uint64_t    last_publish_ms;
    uint64_t    next_attempt_ms;
    uint32_t    connects, publishes;
} BlStatus;

typedef struct {
    void *ctx;
    // Connected, "online" said: subscribe (bl_session_subscribe). Negative:
    // the connection fails and is tried again later.
    int (*subscribe)(void *ctx);
    // The connection went (the status says why), before the log line.
    void (*lost)(void *ctx);
    // The console's user clock, POSIX seconds: tells a console that slept.
    // May be NULL (never asked).
    uint64_t (*now_posix)(void *ctx);
    // One line for the log. May be NULL.
    void (*log)(void *ctx, const char *line);
} BlSessionHost;

// Where and who. The strings are the caller's and must outlive the session
// (or until the next bl_session_configure).
typedef struct {
    const char *host;
    uint16_t    port;
    const char *username;    // "" or NULL: none
    const char *password;    // sent only with a user name
    uint8_t     version;     // MQTT_V5, MQTT_V311, or 0: 5.0 then 3.1.1 (see above)
    const char *base;        // the topics' root: "<prefix>/<id>"
    const char *client_id;
} BlSessionConf;

typedef struct {
    BlSessionHost host;
    BlSessionConf conf;
    MqttClient    mqtt;
    uint64_t    (*now_ms)(void);
    BlStatus      status;
    uint64_t      backoff_ms;
    uint8_t       proto_ok;      // version 0: the one the broker took (0: try 5.0 first)
    uint64_t      seen_posix;    // the wall clock at the last call (0: not read yet)
    uint8_t       rx[BL_RX_SIZE];
    char          line[256];
} BlSession;

// `on_message` gets every message the subscriptions bring, `user` with it.
void bl_session_init(BlSession *s, const BlSessionConf *conf, BlIo io, const BlSessionHost *host,
                     uint64_t (*now_ms)(void), MqttOnMessage on_message, void *user);
// New broker or identity: takes effect at the next connection (the caller
// stops the session first when it must reconnect). Version 0 tries 5.0 again
// when `forget_version`.
void bl_session_configure(BlSession *s, const BlSessionConf *conf, bool forget_version);

// The console slept past the broker's patience: the connection is closed at
// once, without a word (the broker has said "offline" for it already), and
// true returned. Every call that writes asks first.
bool bl_session_woke(BlSession *s);
// Connects when due. Negative: online, carry on (`connected` true when it
// just connected). Else how long the caller may sleep before the next turn.
int  bl_session_connect_due(BlSession *s, int max_wait_ms, bool *connected);
// After the caller's work: a connection that dropped meanwhile is told lost,
// to be tried again after the shortest wait.
bool bl_session_check(BlSession *s);
// The connection is gone: the status says why, the next attempt waits the
// current pause, the host and the log are told.
void bl_session_lost(BlSession *s);
// Waits for the broker up to `wait_ms`, handing what comes to `on_message`.
void bl_session_poll(BlSession *s, int wait_ms);
// Lets the next attempt come at once (new settings).
void bl_session_retry_now(BlSession *s);
// "offline" on <base>/availability, DISCONNECT, closed.
void bl_session_stop(BlSession *s);
bool bl_session_online(const BlSession *s);

// Publishes `<base>/<sub>`, counted in the status. 0 or negative.
int  bl_session_publish(BlSession *s, const char *sub, const void *payload, size_t len, bool retained);
// Publishes `<base>/<sub>`, `len` bytes that `read` gives a piece at a time
// into `buf` (a file sent without holding it whole). A read that fails or
// comes short closes the connection, the packet cannot be finished: the next
// turn connects again. 0 or negative.
typedef size_t (*BlReadFn)(void *ctx, void *buf, size_t cap);
int  bl_session_publish_stream(BlSession *s, const char *sub, size_t len, bool retained, BlReadFn read, void *ctx,
                               void *buf, size_t cap);
// Publishes a whole topic (another root than <base>), not counted. 0 or negative.
int  bl_session_publish_topic(BlSession *s, const char *topic, const void *payload, size_t len, bool retained);
// "online" on <base>/availability again (another client of the same console
// may have left "offline" behind). 0 or negative.
int  bl_session_announce(BlSession *s);
// Subscribes to `count` filters at once; the first `required` must be
// granted. Negative: failed or refused; else how many optional ones the
// broker refused.
int  bl_session_subscribe(BlSession *s, const char *const *filters, const uint8_t *qos, size_t count,
                          size_t required);

#ifdef __cplusplus
}
#endif
