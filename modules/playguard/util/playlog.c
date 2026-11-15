// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "playlog.h"

#include <string.h>

typedef struct {
    PlayLogTotal *out;
    size_t        count, max;
    uint64_t      now;
    uint64_t      day_starts[7];
} Fold;

static uint64_t overlap(uint64_t a, uint64_t b, uint64_t lo, uint64_t hi)
{
    const uint64_t from = a > lo ? a : lo;
    const uint64_t to   = b < hi ? b : hi;
    return to > from ? to - from : 0;
}

// Adds the play time in [start, end_user).
static void add_range(Fold *f, uint64_t app_id, uint64_t start, uint64_t end_user)
{
    const uint64_t week  = overlap(start, end_user, f->day_starts[6], f->now);
    if (week == 0) return;

    size_t i = 0;
    while (i < f->count && f->out[i].app_id != app_id) i++;
    if (i == f->count) {
        if (f->count == f->max) return;
        PlayLogTotal *t = &f->out[i];
        t->app_id = app_id;
        t->today_s = t->week_s = 0;
        for (int k = 0; k < 7; k++) t->day_s[k] = 0;
        f->count++;
    }
    PlayLogTotal *t = &f->out[i];
    t->week_s += (uint32_t)week;
    for (int k = 0; k < 7; k++) {   // a session across midnight counts on both days
        const uint64_t end = k == 0 ? f->now : f->day_starts[k - 1];
        t->day_s[k] += (uint32_t)overlap(start, end_user, f->day_starts[k], end);
    }
    t->today_s = t->day_s[0];
}

// Duration between two events: the steady clock when both have it and it
// went forward, else the user clock; 0 when neither makes sense.
static uint64_t duration(const PlayLogEvent *from, const PlayLogEvent *to)
{
    if (from->ts_steady && to->ts_steady && to->ts_steady >= from->ts_steady)
        return to->ts_steady - from->ts_steady;
    if (to->ts_user >= from->ts_user) return to->ts_user - from->ts_user;
    return 0;
}

// Calls `range(ctx, app_id, start, end)` for each stretch of play in the log
// (user clock, oldest first), a game still in focus counting until `now`.
typedef void (*RangeFn)(void *ctx, uint64_t app_id, uint64_t start, uint64_t end);

// A stretch that ended at `end_user` and lasted `dur` seconds.
static void played(RangeFn range, void *ctx, uint64_t app_id, uint64_t end_user, uint64_t dur)
{
    if (dur == 0 || dur > PLAYLOG_MAX_SESSION_S || end_user < dur) return;
    range(ctx, app_id, end_user - dur, end_user);
}

static void each_range(const PlayLogEvent *events, size_t n, uint64_t now, RangeFn range, void *ctx)
{
    const PlayLogEvent *open = NULL;   // the focus event of the game being played
    for (size_t i = 0; i < n; i++) {
        const PlayLogEvent *e = &events[i];
        switch (e->kind) {
            case PlayLogEv_Focus:
                if (open && open->app_id == e->app_id) break;   // repeated: keep the first
                if (open) played(range, ctx, open->app_id, e->ts_user, duration(open, e));
                open = e->app_id ? e : NULL;
                break;
            case PlayLogEv_Unfocus:
            case PlayLogEv_Exit:
                if (!open || open->app_id != e->app_id) break;   // not the game in focus
                played(range, ctx, open->app_id, e->ts_user, duration(open, e));
                open = NULL;
                break;
            case PlayLogEv_Away:
            case PlayLogEv_Launch:
                if (open) played(range, ctx, open->app_id, e->ts_user, duration(open, e));
                open = NULL;
                break;
            default:
                break;
        }
    }
    // Still in focus (PlayGuard started over that game, for instance).
    if (open && now >= open->ts_user) played(range, ctx, open->app_id, now, now - open->ts_user);
}

static void fold_range(void *ctx, uint64_t app_id, uint64_t start, uint64_t end)
{
    add_range((Fold *)ctx, app_id, start, end);
}

void playlog_now(const PlayLogEvent *events, size_t n, uint64_t *focused, uint64_t *since, uint64_t *waiting)
{
    *focused = *since = *waiting = 0;
    // The newest focus change says which game is in front.
    for (size_t i = n; i-- > 0;) {
        const uint8_t k = events[i].kind;
        if (k == PlayLogEv_AccountOpen || k == PlayLogEv_AccountClose) continue;
        if (k == PlayLogEv_Focus) {
            *focused = events[i].app_id;
            *since = events[i].ts_user;
        }
        break;
    }
    if (*focused) return;
    // None: the newest application's own event says whether it waits.
    for (size_t i = n; i-- > 0;) {
        const uint8_t k = events[i].kind;
        if (k == PlayLogEv_AccountOpen || k == PlayLogEv_AccountClose || k == PlayLogEv_Away) continue;
        if (k == PlayLogEv_Unfocus) *waiting = events[i].app_id;
        break;
    }
}

size_t playlog_fold_days(const PlayLogEvent *events, size_t n, uint64_t now,
                         const uint64_t day_starts[7], PlayLogTotal *out, size_t max)
{
    Fold f;
    f.out = out;
    f.count = 0;
    f.max = max;
    f.now = now;
    for (int k = 0; k < 7; k++) f.day_starts[k] = day_starts[k];
    each_range(events, n, now, fold_range, &f);
    return f.count;
}

size_t playlog_fold_since(const PlayLogEvent *events, size_t n, uint64_t now, uint64_t from,
                          PlayLogTotal *out, size_t max)
{
    // One "day" from `from` to now: its time is the window's.
    uint64_t starts[7];
    for (int k = 0; k < 7; k++) starts[k] = from;
    const size_t count = playlog_fold_days(events, n, now, starts, out, max);
    for (size_t i = 0; i < count; i++) {
        out[i].today_s = 0;
        for (int k = 0; k < 7; k++) out[i].day_s[k] = 0;
    }
    return count;
}

typedef struct {
    PlayLogHours  *out;
    uint64_t       now;
    const uint64_t *day_starts;
    uint64_t       from, to;   // the session being put together
    int            day;        // the day it started on (days back)
} HourFold;

static void close_session(HourFold *h)
{
    if (h->to <= h->from) return;
    const uint64_t len = h->to - h->from;
    h->out->sessions++;
    h->out->session_s += (uint32_t)len;
    if (len > h->out->longest_s) {
        h->out->longest_s   = (uint32_t)len;
        h->out->longest_day = (uint8_t)h->day;
    }
}

static void hour_range(void *ctx, uint64_t app_id, uint64_t start, uint64_t end)
{
    (void)app_id;
    HourFold *h = (HourFold *)ctx;
    const uint64_t *ds = h->day_starts;
    if (end > h->now) end = h->now;
    if (start < ds[6]) start = ds[6];
    if (end <= start) return;
    // Each hour of each day it covers.
    for (int k = 0; k < 7; k++) {
        const uint64_t day_end = k == 0 ? h->now : ds[k - 1];
        for (int hr = 0; hr < 24; hr++) {
            const uint64_t lo = ds[k] + (uint64_t)hr * 3600u;
            // The last hour runs to the day's end (a 25 h day, daylight saving).
            const uint64_t hi = hr == 23 ? day_end : lo + 3600u;
            if (lo >= day_end) break;
            h->out->hour_s[k][hr] += (uint32_t)overlap(start, end, lo, hi);
        }
    }
    // The sessions: ranges less than PLAYLOG_SESSION_GAP_S apart are one.
    if (h->to && start <= h->to + PLAYLOG_SESSION_GAP_S) {
        if (end > h->to) h->to = end;
        return;
    }
    close_session(h);
    h->from = start;
    h->to   = end;
    h->day  = 6;
    while (h->day > 0 && start >= ds[h->day - 1]) h->day--;
}

size_t playlog_account_days(const PlayLogEvent *events, size_t n, uint64_t now, const uint64_t day_starts[7],
                            PlayLogEvent *scratch, PlayLogTotal *totals, size_t totals_max,
                            PlayLogAccountDays *out, size_t max)
{
    size_t count = 0;
    for (size_t i = 0; i < n && count < max; i++) {
        if (events[i].kind != PlayLogEv_AccountOpen || (!events[i].uid[0] && !events[i].uid[1])) continue;
        bool known = false;
        for (size_t a = 0; a < count && !known; a++)
            known = out[a].uid[0] == events[i].uid[0] && out[a].uid[1] == events[i].uid[1];
        if (known) continue;
        PlayLogAccountDays *a = &out[count++];
        memset(a, 0, sizeof(*a));
        a->uid[0] = events[i].uid[0];
        a->uid[1] = events[i].uid[1];
        const size_t m = playlog_for_account(events, n, a->uid, scratch, n);
        const size_t games = playlog_fold_days(scratch, m, now, day_starts, totals, totals_max);
        for (size_t g = 0; g < games; g++)
            for (int k = 0; k < 7; k++) a->day_s[k] += totals[g].day_s[k];
    }
    return count;
}

void playlog_fold_hours(const PlayLogEvent *events, size_t n, uint64_t now,
                        const uint64_t day_starts[7], PlayLogHours *out)
{
    memset(out, 0, sizeof(*out));
    HourFold h;
    h.out = out;
    h.now = now;
    h.day_starts = day_starts;
    h.from = h.to = 0;
    h.day = 0;
    each_range(events, n, now, hour_range, &h);
    close_session(&h);
}

// An event of `kind` for `app_id` at the time of `at`.
static void emit(PlayLogEvent *out, size_t *count, size_t max, uint8_t kind, uint64_t app_id, const PlayLogEvent *at)
{
    if (*count == max) return;
    PlayLogEvent *e = &out[(*count)++];
    e->app_id    = app_id;
    e->kind      = kind;
    e->ts_user   = at->ts_user;
    e->ts_steady = at->ts_steady;
    e->uid[0] = e->uid[1] = 0;
}

size_t playlog_for_account(const PlayLogEvent *events, size_t n, const uint64_t uid[2],
                           PlayLogEvent *out, size_t max)
{
    size_t count = 0;
    uint64_t focused = 0;       // the game with the focus, 0 for none
    bool     open    = false;   // `uid` is open in a game
    bool     playing = false;   // a Focus was emitted and not closed yet

    for (size_t i = 0; i < n; i++) {
        const PlayLogEvent *e = &events[i];
        const bool mine = e->uid[0] == uid[0] && e->uid[1] == uid[1];
        switch (e->kind) {
            case PlayLogEv_Focus:
                if (focused == e->app_id && playing) break;   // repeated
                // Another game's focus ends the one played (playlog_fold_days):
                // one event out per event in, so `out` may be `events`.
                if (playing && !(open && e->app_id)) emit(out, &count, max, PlayLogEv_Away, 0, e);
                focused = e->app_id;
                playing = open && focused;
                if (playing) emit(out, &count, max, PlayLogEv_Focus, focused, e);
                break;
            case PlayLogEv_Unfocus:
            case PlayLogEv_Exit:
                if (focused != e->app_id) break;   // not the game in focus
                if (playing) emit(out, &count, max, PlayLogEv_Unfocus, focused, e);
                focused = 0;
                playing = false;
                break;
            case PlayLogEv_Away:
            case PlayLogEv_Launch:
                if (playing) emit(out, &count, max, PlayLogEv_Away, 0, e);
                focused = 0;
                playing = false;
                if (e->kind == PlayLogEv_Launch) open = false;   // the previous game's accounts
                break;
            case PlayLogEv_AccountOpen:
                if (!mine) break;
                open = true;
                // Selected while the game already has the focus (the user
                // picker comes after the launch): it counts from now.
                if (focused && !playing) {
                    emit(out, &count, max, PlayLogEv_Focus, focused, e);
                    playing = true;
                }
                break;
            case PlayLogEv_AccountClose:
                if (!mine) break;
                open = false;
                if (playing) emit(out, &count, max, PlayLogEv_Unfocus, focused, e);
                playing = false;
                break;
            default:
                break;
        }
    }
    return count;
}
