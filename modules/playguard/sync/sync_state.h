// sync_state — the JSON documents the console publishes: the state
// snapshot (`<prefix>/<id>/state`), today's activity and a finished day's
// totals (`activity`, `activity/<date>`), the names table (`names`) and the
// events (`event`). See docs/sync-protocol.md. Every object of the state is
// always present; only its leaves are null when the console could not say,
// so a Home Assistant template never fails on a missing level.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "../core/pctl_ops.h"
#include "core/sysinfo.h"
#include "sync_conf.h"
#include "sync_exec.h"
#include "sync_relay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_SCHEMA 1

// What a title is (sync_titles.h): only games are listed (names, games,
// icons, per_app); the one in front is named whatever it is.
typedef enum {
    SyncTitle_Game = 0,
    SyncTitle_Homebrew,   // an NRO forwarder (another app's, or one PlayGuard made)
    SyncTitle_System,     // the console's own programs
    SyncTitle_PlayGuard,  // PlayGuard's own forwarder: a homebrew, told apart
} SyncTitleKind;

// What the console is doing when no title has the focus (activity_today.now):
// with one, its kind says.
typedef enum {
    SyncNow_Unknown = 0,   // null: the play log unread
    SyncNow_PlayGuard,     // PlayGuard in front (it publishes the state itself)
    SyncNow_Home,          // the HOME menu or a system applet, nothing waiting
    SyncNow_Suspended,     // the same, with a game (or homebrew) waiting in the background
} SyncNow;

typedef struct {
    const char        *source;          // "agent" or "app"
    uint64_t           ts;              // user clock, POSIX seconds
    const char        *local_date;      // "YYYY-MM-DD", NULL or "" when unknown
    int                weekday;         // 0 = Sunday, -1 unknown
    bool               local_min_ok;    // local_min is known
    int                local_min;       // minutes since local midnight
    bool               clock_accurate_ok, clock_accurate;

    const SyncConf    *conf;            // id, name, policy, switches
    const SysInfo     *sys;             // NULL: unknown
    const char        *nickname;        // System Settings' console nickname (its name when sync.conf names none); NULL unknown
    const char        *app_version;     // NULL or "": unknown
    const char        *agent_version;   // NULL or "": no agent
    bool               read_only;

    const PctlStatus  *status;          // NULL: not read
    const PtState     *timer;           // NULL: not read
    bool               spent_ok;        // 1952
    uint64_t           spent_ns;
    const SyncRecords *records;         // NULL: none

    bool               activity_ok;     // today's play-log total
    uint32_t           activity_s;
    uint64_t           now_playing;     // application id, 0: none
    uint64_t           now_playing_since;
    const char        *now_playing_name;
    SyncTitleKind      now_playing_kind;
    SyncNow            now;                // with no title in front

    bool               agent;           // the agent sysmodule runs (and publishes)
    const char        *last_result;     // "limit_mon: applied", NULL or "": none

    const char       (*profiles)[SYNC_PROFILE_MAX];   // the saved profiles' names (the profile order's choices)
    size_t             n_profiles;
    const char        *profile_now;     // the saved profile whose seven limits are the timer's; NULL none
} SyncSnapshot;

size_t sync_state_build(const SyncSnapshot *s, char *out, size_t cap);
// Why play is blocked: "bedtime" within the bedtime alarm's hours (from the
// alarm to the time play is allowed again), else "limit" (the day's limit,
// or No more play today). NULL: not blocked, or unknown.
const char *sync_blocked_by(const SyncSnapshot *s);
// Minutes left of today's limit (0 once games are blocked). false: no limit
// today, the timer off, or unknown.
bool sync_remaining_min(const SyncSnapshot *s, int64_t *left);

// Time played per application (today's, or a finished day's).
typedef struct {
    uint64_t app_id;
    uint32_t seconds;
} SyncAppTime;

// Time played per user account.
typedef struct {
    uint64_t uid[2];
    uint32_t seconds;
} SyncAccountTime;

typedef struct {
    const char            *source;
    uint64_t               ts;
    const char            *local_date;
    bool                   final;           // a finished day (activity/<date>)
    const SyncAppTime     *apps;
    size_t                 n_apps;
    const SyncAccountTime *accounts;        // NULL: not known
    size_t                 n_accounts;
    uint64_t               now_playing;     // not for a finished day
    uint64_t               now_playing_since;
    SyncTitleKind          now_playing_kind;
    // The time of the titles that are not games (left out of `apps`): in
    // total_s, told apart as homebrew_s.
    uint32_t               homebrew_s;
} SyncActivity;

size_t sync_activity_build(const SyncActivity *a, char *out, size_t cap);

typedef struct {
    uint64_t    app_id;
    const char *name;
} SyncAppName;

typedef struct {
    uint64_t    uid[2];
    const char *nickname;
} SyncAccountName;

size_t sync_names_build(uint64_t ts, const SyncAppName *apps, size_t n_apps, const SyncAccountName *accounts,
                        size_t n_accounts, char *out, size_t cap);

// One event (`event_type`, the order's entity and payload, why not, the
// result code), as the `event` topic and HA's event entity take it.
size_t sync_event_build(const char *source, uint64_t ts, const char *event_type, const char *entity,
                        const char *payload, SyncReason reason, bool rc_set, uint32_t rc, char *out, size_t cap);

// What the console is and which homebrews publish on it, retained on
// <base>/capabilities (docs/sync-protocol.md): the platform, and PlayGuard
// with the schema of its documents and its version when its module is on
// (`playguard`: false on an agent that runs without PlayGuard), then the
// homebrews that relay.
#define SYNC_PLATFORM "switch"
#define SYNC_CAPABILITIES_SCHEMA 1
size_t sync_capabilities_build(const char *sw_version, bool playguard, const SyncRelayModule *relayed,
                               size_t n_relayed, char *out, size_t cap);

// Minutes played today by the console timer's own count (today's limit minus
// 1454), or -1 when it does not say (pt_logic::played_today_min).
int sync_played_today_min(const PtState *pt, int weekday);

// "none", "custom", "young_child", "child", "teen", or "" outside them.
const char *sync_level_name(uint32_t level);

#ifdef __cplusplus
}
#endif
