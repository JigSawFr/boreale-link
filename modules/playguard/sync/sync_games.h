// sync_games — the games document (docs/sync-protocol.md, `games`): the
// games installed and played, each with its play time since it was first
// played, its launches and when; the number installed, of user accounts, and
// the console's play time in all. Built by PlayGuard (the play log, the
// installed list) and relayed by the agent; unknown values are null.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t    app_id;
    const char *name;           // NULL unknown
    const char *version;        // the installed version ("1.3.0"); NULL unknown or not installed
    bool        installed;
    int64_t     total_min;      // since it was first played; -1 unknown
    int64_t     launches;       // -1 unknown
    uint64_t    first_played;   // POSIX; 0 unknown
    uint64_t    last_played;
} SyncGame;

typedef struct {
    uint64_t        ts;
    int             installed;   // games installed; -1 unknown
    int             accounts;    // user accounts; -1 unknown
    int64_t         total_min;   // every title played, every account (the homebrews' too); -1 unknown
    const SyncGame *games;       // games only, most played first: the ones that do not fit are left out
    size_t          n_games;
    // The homebrews, as counts only (no ID, no name): the .nro files on the
    // SD card, the forwarders installed (PlayGuard's own among them), and
    // their play time since first played, in total_min. -1 unknown.
    int             homebrews;
    int             forwarders;
    int64_t         homebrew_min;
} SyncGames;

// The document; `*kept` (may be NULL) gets the number of games it holds.
size_t sync_games_build(const SyncGames *g, char *out, size_t cap, size_t *kept);

#ifdef __cplusplus
}
#endif
