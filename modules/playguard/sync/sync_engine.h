// sync_engine — the remote link's PlayGuard side, shared by PlayGuard and the
// agent sysmodule: on the broker session (link/bl_session: the connection,
// the last will "offline", the retries), subscribes to the orders, publishes
// the state, today's activity and Home Assistant's discovery, and hands each
// order to the host, which carries it out on the console. Single-threaded and allocation-free: the host owns one SyncEngine
// (about 40 KiB) and calls sync_engine_step() in a loop.
//
// An order's life: received on `<base>/<entity>/set` (retained by whoever
// sent it, so it waits on the broker while the console sleeps) -> parsed ->
// refused at once (unknown, out of range, policy, remote_timer_writes) or
// handed to the host -> an `event` says applied or why not -> the retained
// order is cleared (an empty retained publish), so it is used once -> the
// state is published again. Several limit orders arriving together (Home
// Assistant's sliders) are merged into one write after a short pause.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "bl_session.h"
#include "sync_apply.h"
#include "sync_conf.h"
#include "sync_discovery.h"
#include "sync_exec.h"
#include "sync_notice.h"
#include "sync_relay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_QUEUE_MAX       16
#define SYNC_RX_SIZE         BL_RX_SIZE
#define SYNC_SCRATCH_SIZE    32768   // the discovery: 18.5 KiB with every entity
#define SYNC_LIMIT_SETTLE_MS 1500
#define SYNC_KEEPALIVE_S     BL_KEEPALIVE_S
// The integration's document on <base>/integration: larger ones are dropped.
#define SYNC_INTEGRATION_MAX 512

typedef enum {
    SyncLink_Off = BlLink_Off,               // disabled, or not configured
    SyncLink_Waiting = BlLink_Waiting,       // waiting before the next attempt
    SyncLink_Connecting = BlLink_Connecting,
    SyncLink_Online = BlLink_Online,
} SyncLinkState;

typedef struct {
    SyncLinkState state;
    uint8_t       protocol;           // MQTT_V5 or MQTT_V311 once connected (the last one), 0 never
    char          error[128];         // the last failure ("" none)
    uint64_t      online_since_ms;
    uint64_t      last_publish_ms;
    uint64_t      next_attempt_ms;
    uint32_t      connects, publishes, orders, rejected, dropped;
    char          last_result[SYNC_ENTITY_MAX + 40];   // "limit_mon: applied"
} SyncStatus;

typedef struct {
    void *ctx;
    // The state document (sync_state_build). 0: nothing to publish now.
    size_t (*state)(void *ctx, char *out, size_t cap);
    // Today's activity (sync_activity_build). 0: none now. May be NULL.
    size_t (*activity)(void *ctx, char *out, size_t cap);
    // An order for the console. true: `out` is final. false: the host took it
    // (another thread, PlayGuard through the agent) and calls
    // sync_engine_order_done(id, …) later.
    bool (*order)(void *ctx, uint32_t id, const SyncIntent *intent, const char *entity, const char *payload,
                  bool retained, SyncOutcome *out);
    // Export a diagnostic report: saves it and returns its text (published
    // when publish_report is on). 0: failed. NULL: the order goes to
    // `order` instead (the host writes the report and publishes it itself).
    size_t (*report)(void *ctx, char *out, size_t cap);
    // The saved profiles, for the discovery's select. May be NULL.
    size_t (*profiles)(void *ctx, char (*names)[SYNC_PROFILE_MAX], size_t max);
    // An order changed the settings (ha_discovery): save them. May be NULL.
    void (*conf_changed)(void *ctx, const SyncConf *conf);
    // PlayGuard's read-only mode. May be NULL (never read-only).
    bool (*read_only)(void *ctx);
    // The console's user clock, POSIX seconds (events' ts).
    uint64_t (*now_posix)(void *ctx);
    // One line for the log. May be NULL.
    void (*log)(void *ctx, const char *line);
    // The Home Assistant integration's document, retained on <base>/integration
    // ({"schema":1,"version":"0.2.0","homeassistant":"2026.10.1"}), as it came:
    // at each connection and when the integration changes it; len 0 when it
    // was cleared (the integration removed). Not NUL-terminated, at most
    // SYNC_INTEGRATION_MAX bytes. May be NULL.
    void (*integration)(void *ctx, const char *doc, size_t len);
    // The system document (sync_system_build), published with the state.
    // 0: nothing now. May be NULL.
    size_t (*system)(void *ctx, char *out, size_t cap);
    // What the console is (model, system), for the discovery's device; read
    // again with each system document, the discovery following a change. May
    // be NULL (unknown).
    void (*device)(void *ctx, SyncDevice *out);
    // The notices document (sync_notices_build): every Boréale homebrew's
    // shared bell, published with the state when it changed, and at each
    // connection. 0: nothing now. May be NULL.
    size_t (*notices)(void *ctx, char *out, size_t cap);
    // An order for another homebrew's module, relayed through this engine:
    // `rel` is the topic below the base (`<module>/<sub>`, ending in "/set"),
    // never one of PlayGuard's own. The host hands it on (boreale-link's
    // bl_relay decides whether the module may relay). May be NULL: nothing
    // of the kind is subscribed to.
    void (*relay)(void *ctx, const char *rel, const void *payload, size_t len, bool retained);
} SyncHost;

#define SYNC_RELAY_MODULES    4
#define SYNC_RELAY_MODULE_MAX 33   // boreale-link's BL_APP_MODULE_MAX

typedef struct {
    char     entity[SYNC_ENTITY_MAX];
    char     payload[SYNC_PAYLOAD_MAX];
    bool     retained;
    uint32_t id;          // 0: free slot
    uint32_t group;       // the order whose console call carries this one
    bool     in_flight;
    uint64_t received_ms;
} SyncQueued;

typedef struct {
    SyncConf    conf;
    SyncHost    host;
    BlSession   session;         // the broker: connection, last will, retries
    uint64_t  (*now_ms)(void);
    const char *source;          // "agent" or "app"
    const char *sw_version;
    char        base[96];        // <prefix>/<id>
    char        client_id[24];

    bool        want_state, want_activity, want_discovery, want_names, want_online;
    bool        want_clear;      // the documents of what is not published, cleared
    bool        want_capabilities;
    bool        no_module;       // PlayGuard's module is off (sync_engine_set_module): the console's own documents only
    uint32_t    notices_hash;    // the notices document last published (0: publish the next one)
    // The state and system documents last published: the hash and when. An
    // identical one is not sent again before STATE_REPUBLISH_MS (0: publish the next one).
    uint32_t    state_hash, system_hash;
    uint64_t    state_pub_ms, system_pub_ms;
    uint64_t    next_state_ms, next_activity_ms;
    bool        discovery_published;
    SyncDevice  device;          // as the last discovery said it

    SyncQueued  queue[SYNC_QUEUE_MAX];
    uint32_t    next_id;
    // Orders already reported as waiting (no new event when the broker
    // resends them): entity + payload hash.
    uint32_t    waiting[SYNC_QUEUE_MAX];

    // What the console did on its own (sync_watch), published after the state.
    SyncNotice  notices[SYNC_NOTICE_MAX];
    size_t      n_notices;

    // The modules of other homebrews whose orders are subscribed to (relay).
    char        relay_modules[SYNC_RELAY_MODULES][SYNC_RELAY_MODULE_MAX];
    char        relay_versions[SYNC_RELAY_MODULES][24];
    bool        relay_open[SYNC_RELAY_MODULES];   // its session is open: it is in `capabilities`
    size_t      n_relay_modules;

    SyncStatus  status;
    char        profiles[16][SYNC_PROFILE_MAX];
    char        scratch[SYNC_SCRATCH_SIZE];
    char        line[320];
} SyncEngine;

void sync_engine_init(SyncEngine *e, const SyncConf *conf, BlIo io, const SyncHost *host, uint64_t (*now_ms)(void),
                      const char *source, const char *sw_version);
// New settings: reconnects when the broker or the identity changed, else
// publishes the discovery again.
void sync_engine_reconfigure(SyncEngine *e, const SyncConf *conf);
// One turn: connects when due, handles orders, publishes what is due, then
// waits for the broker for up to `max_wait_ms`. Returns how long the caller
// may sleep before the next turn (0: call again now).
int  sync_engine_step(SyncEngine *e, int max_wait_ms);
// Publish everything again at the next turn (Sync now, Home Assistant restarted).
void sync_engine_sync_now(SyncEngine *e);
void sync_engine_state_changed(SyncEngine *e);       // the state, at the next turn
void sync_engine_activity_changed(SyncEngine *e);    // today's activity, at the next turn
void sync_engine_discovery_changed(SyncEngine *e);   // the profiles changed
// The outcome of an order the host took (SyncHost.order returned false).
void sync_engine_order_done(SyncEngine *e, uint32_t id, const SyncOutcome *out);
// Asks the broker for the retained orders again (re-subscribing resends
// them): the orders kept waiting for PlayGuard come back.
void sync_engine_replay_orders(SyncEngine *e);
// Publishes `<base>/<sub>` (retained or not). 0 or negative.
// Whether PlayGuard's module runs (the default). An agent without PlayGuard
// turns it off: it then publishes the console's own documents (`system`,
// `notices`, `capabilities`, the relayed homebrews') but neither PlayGuard's
// state, activity and discovery, nor does it subscribe to or act on its
// orders, and `capabilities` does not list it. Turned on again, everything is
// published again and the orders kept by the broker come back. Cheap when
// nothing changed.
void sync_engine_set_module(SyncEngine *e, bool on);
// The homebrews that relay through the agent whose session is open now
// (`open`, `n_open` of them; the module's name and its version): each one's
// orders are subscribed to (`<base>/<module>/+/set`, now when connected and at
// each connection; the subscription stays when the session ends), and
// `capabilities` lists exactly these, published again when the set or a
// version changed. PlayGuard's own module, an unusable name or no room for one
// more leaves that module out. Called each turn, cheap when nothing changed.
void sync_engine_relay_sync(SyncEngine *e, const SyncRelayModule *open, size_t n_open);
// Publishes `<base>/<rel>` for another homebrew's module (`rel` is
// `<module>/<sub>`, already checked by the relay). PlayGuard's own level and
// the console's are refused: -1.
int  sync_engine_publish_relay(SyncEngine *e, const char *rel, const void *payload, size_t len, bool retained);
int  sync_engine_publish(SyncEngine *e, const char *sub, const char *payload, size_t len, bool retained);
// Publishes `<base>/<sub>`, `len` bytes that `read` gives a piece at a time
// (into the engine's scratch: a file sent without holding it whole). A read
// that fails or comes short closes the connection, the packet cannot be
// finished: the next step connects again. 0 or negative.
typedef BlReadFn SyncReadFn;
int  sync_engine_publish_stream(SyncEngine *e, const char *sub, size_t len, bool retained, SyncReadFn read, void *ctx);
// An event (`event_type`, optional entity) on `<base>/event`.
void sync_engine_event(SyncEngine *e, const char *event_type, const char *entity);
// What the console did on its own (sync_watch_step), published on
// `<base>/event` after the state document; dropped while offline (events
// are not retained: a consumer that was away misses them).
void sync_engine_notices(SyncEngine *e, const SyncNotice *n, size_t count);
// "offline", DISCONNECT, closed. sync_engine_step() reconnects later unless
// the settings turn the link off.
void sync_engine_stop(SyncEngine *e);
bool sync_engine_online(const SyncEngine *e);
const SyncStatus *sync_engine_status(const SyncEngine *e);
size_t sync_engine_pending(const SyncEngine *e);

#ifdef __cplusplus
}
#endif
