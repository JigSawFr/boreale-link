// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_games.h"

#include <stdio.h>

#include "bl_json.h"
#include "sync_state.h"   // SYNC_SCHEMA

// What one game takes at most: its id, a long name, the numbers.
#define GAME_ROOM 640

size_t sync_games_build(const SyncGames *g, char *out, size_t cap, size_t *kept)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kint_or_null(&j, "ts", g->ts != 0, (int64_t)g->ts);
    bl_json_kint_or_null(&j, "installed", g->installed >= 0, g->installed);
    bl_json_kint_or_null(&j, "accounts", g->accounts >= 0, g->accounts);
    bl_json_kint_or_null(&j, "total_min", g->total_min >= 0, g->total_min);
    bl_json_kint_or_null(&j, "homebrews", g->homebrews >= 0, g->homebrews);
    bl_json_kint_or_null(&j, "forwarders", g->forwarders >= 0, g->forwarders);
    bl_json_kint_or_null(&j, "homebrew_min", g->homebrew_min >= 0, g->homebrew_min);
    size_t n = 0;
    bl_json_key(&j, "games");
    bl_json_obj(&j);
    for (; n < g->n_games; n++) {
        if (j.len + GAME_ROOM + 16 > cap) break;   // the rest does not fit: the least played
        const SyncGame *e = &g->games[n];
        char id[17];
        snprintf(id, sizeof(id), "%016llX", (unsigned long long)e->app_id);
        bl_json_key(&j, id);
        bl_json_obj(&j);
        bl_json_kstr_or_null(&j, "name", e->name);
        bl_json_kstr_or_null(&j, "version", e->version);
        bl_json_kbool(&j, "installed", e->installed);
        bl_json_kint_or_null(&j, "total_min", e->total_min >= 0, e->total_min);
        bl_json_kint_or_null(&j, "launches", e->launches >= 0, e->launches);
        bl_json_kint_or_null(&j, "first_played", e->first_played != 0, (int64_t)e->first_played);
        bl_json_kint_or_null(&j, "last_played", e->last_played != 0, (int64_t)e->last_played);
        bl_json_obj_end(&j);
    }
    bl_json_obj_end(&j);
    bl_json_kbool(&j, "complete", n == g->n_games);
    bl_json_obj_end(&j);
    if (kept) *kept = n;
    return bl_json_end(&j);
}
