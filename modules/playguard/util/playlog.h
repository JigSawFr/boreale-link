// playlog — play time per game for today and the last 7 days, folded from the
// console's play-event log (pdm:qry QueryPlayEvent). Plain C with no libnx, so
// the host tests can feed it made-up logs; source/core/playstats.c reads the
// real one.
//
// How the log is read: a game is played from the moment it gets the focus
// until it loses it (out of focus, exit), or until something else takes over
// without that being logged for the game (the HOME menu gets the focus after a
// crash, the console sleeps or shuts down, another game starts or gets the
// focus). Durations come from the steady clock, so changing the console clock
// (Console › Network clock) does not stretch them; the user clock only places them
// on the calendar.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PlayLogEv_Focus        = 0,   // `app_id` gets the focus
    PlayLogEv_Unfocus      = 1,   // `app_id` loses it, or exits
    PlayLogEv_Away         = 2,   // whatever game had the focus no longer has it
    PlayLogEv_AccountOpen  = 3,   // user account `uid` is selected in the game
    PlayLogEv_AccountClose = 4,   // user account `uid` is closed by the game
    PlayLogEv_Launch       = 5,   // a game starts: as Away, and the accounts the
                                  // previous one had open are closed with it
    PlayLogEv_Exit         = 6,   // `app_id` exits: as Unfocus for the time played; it
                                  // no longer waits in the background either
} PlayLogKind;

typedef struct {
    uint64_t app_id;      // 0 for PlayLogEv_Away and the account events
    uint8_t  kind;        // PlayLogKind
    uint64_t ts_user;     // POSIX seconds, user clock (what "today" refers to)
    uint64_t ts_steady;   // seconds, steady clock (0 when unknown)
    uint64_t uid[2];      // the account events' user account
} PlayLogEvent;

typedef struct {
    uint64_t app_id;
    uint32_t today_s;     // seconds played in [day_start, now)
    uint32_t week_s;      // seconds played in [week_start, now)
    uint32_t day_s[7];    // per day: [0] today, [1] yesterday … [6] six days ago
} PlayLogTotal;

// A session longer than this is a broken log (missing events), not play time.
#define PLAYLOG_MAX_SESSION_S (24u * 3600u)

// Folds `events` (oldest first) into one total per game with time in the
// week window. `now` is the current user-clock time; a game still in focus at
// the end of the log counts until then. Returns the number of totals written
// to `out` (games beyond `max` are dropped). `day_starts[k]` is when the day k
// days back began (strictly decreasing: [0] today's midnight, [6] the
// window's start). Days are taken as given, so a 23 or 25 h day (a
// daylight-saving change, calendar.h) is one day.
// What the newest events (oldest first) say is in front: `*focused` the
// application with the focus and `*since` when it got it (0: none, the HOME
// menu or an applet in front); `*waiting` an application out of focus that
// has not exited (it waits in the background, suspended), 0 none or one has
// the focus. A game that lost the focus with no event of its own (it
// crashed) waits nowhere.
void playlog_now(const PlayLogEvent *events, size_t n, uint64_t *focused, uint64_t *since, uint64_t *waiting);

size_t playlog_fold_days(const PlayLogEvent *events, size_t n, uint64_t now,
                         const uint64_t day_starts[7], PlayLogTotal *out, size_t max);

// Every game's time in [from, now), in its week_s (today_s and day_s left
// at 0): the same fold over another window (Activity's last 30 days).
size_t playlog_fold_since(const PlayLogEvent *events, size_t n, uint64_t now, uint64_t from,
                          PlayLogTotal *out, size_t max);

// When the play happened over the same 7 days, every game together: the
// seconds in each hour of each day ([k][h]: k days back, hour h of that day,
// counted from its midnight; on a 25 h day the last hour holds two), and the
// sessions: stretches of play less than PLAYLOG_SESSION_GAP_S apart are one
// (a game changed, a short break), counted on the day they start.
#define PLAYLOG_SESSION_GAP_S (5u * 60u)
typedef struct {
    uint32_t hour_s[7][24];
    uint32_t sessions;      // how many
    uint32_t session_s;     // their time together (sessions * average)
    uint32_t longest_s;     // the longest one
    uint8_t  longest_day;   // the day it started, days back
} PlayLogHours;

void playlog_fold_hours(const PlayLogEvent *events, size_t n, uint64_t now,
                        const uint64_t day_starts[7], PlayLogHours *out);

// The log as one user account played it: a game counts for `uid` while it has
// the focus AND that account is open in it (selected when the game started,
// until the game closes it, or until another game is launched). The focus
// events are rewritten at the moments both hold, so playlog_fold_days() on the
// result gives that account's time. Several accounts open at once (local
// multiplayer) each get the whole time, as the system's own per-account
// statistics do. Returns the number of events written to `out` (at most `n`,
// never more than read so far: `out` may be `events`, filtered in place).
size_t playlog_for_account(const PlayLogEvent *events, size_t n, const uint64_t uid[2],
                           PlayLogEvent *out, size_t max);

// Each user account's play time per day over the same 7 days: the accounts
// opened in the log, in the order they first were (at most `max`), each with
// playlog_for_account() then playlog_fold_days() summed over its games.
// `scratch` holds `n` events and `totals` `totals_max` games, both for the
// work. Returns the number of accounts written to `out`.
typedef struct {
    uint64_t uid[2];
    uint32_t day_s[7];    // [0] today … [6] six days ago
} PlayLogAccountDays;

size_t playlog_account_days(const PlayLogEvent *events, size_t n, uint64_t now, const uint64_t day_starts[7],
                            PlayLogEvent *scratch, PlayLogTotal *totals, size_t totals_max,
                            PlayLogAccountDays *out, size_t max);

#ifdef __cplusplus
}
#endif
