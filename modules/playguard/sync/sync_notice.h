// sync_notice — one thing the console did on its own (sync_watch finds it,
// the engine publishes it on `<base>/event`). Apart from sync_watch.h so that
// the engine's users need not see the state documents' types.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *event_type;    // "play_started", "play_stopped", "time_warning", "play_blocked"
    uint64_t    app_id;        // the game (0: none or unknown)
    int         kind;          // its SyncTitleKind (sync_state.h)
    int         minutes_left;  // time_warning: the minutes left; else -1
    const char *blocked_by;    // play_blocked: "limit" or "bedtime"; else NULL
} SyncNotice;

#define SYNC_NOTICE_MAX 4

// One of them as the `event` topic takes it.
size_t sync_notice_build(const char *source, uint64_t ts, const SyncNotice *n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
