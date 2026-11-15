// sync_watch — what the console does on its own, told as events: a game
// started or stopped, a few minutes left of today's limit, games blocked (the
// limit or bedtime). Compares each state snapshot with the last one; the
// agent feeds it (it is the one running while a game is played), and the
// engine publishes what it finds on `<base>/event` beside the orders' events.
// See docs/sync-protocol.md.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "sync_notice.h"
#include "sync_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// The warning: this many minutes left of today's limit, or fewer.
#define SYNC_WARN_MIN 10

typedef struct {
    bool     seen;       // a first snapshot was taken (it says nothing: no event for what was already so)
    uint64_t playing;    // the game being played, 0 none
    int      playing_kind;   // its SyncTitleKind
    bool     blocked;
    bool     warned;     // the warning was given; again only once the time left goes back above SYNC_WARN_MIN
} SyncWatch;

// The events between the last snapshot and this one, in the order they
// happened (a game stopped before another started); at most SYNC_NOTICE_MAX.
size_t sync_watch_step(SyncWatch *w, const SyncSnapshot *s, SyncNotice out[SYNC_NOTICE_MAX]);

#ifdef __cplusplus
}
#endif
