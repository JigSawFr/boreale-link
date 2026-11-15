// playstats_logic — the parts of the play data layer (playstats.c) that never
// talk to a service: one raw log entry as a playlog event, the bounded walk
// of the log through a query function and the names already known (a game's
// name out of its control.nacp is nacp.h's, shared with the installed games
// check; its icon is kept in boreale-link's bl_cache). libnx calls stay in
// playstats.c, so the host tests (tests/playstats/) run all of this against a
// fake <switch.h>.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once
#include "core/nacp.h"
#include "playstats.h"

#ifdef __cplusplus
extern "C" {
#endif

// One raw log entry as a playlog event; false for entries that do not matter.
bool playstats_convert(const PdmPlayEvent *p, PlayLogEvent *e);

// Reads `count` raw entries from `index` (pdmqryQueryPlayEvent's signature).
typedef Result (*PlayEventQuery)(s32 index, PdmPlayEvent *events, s32 count, s32 *got);

#define PLAYSTATS_EVENT_CHUNK 256
// At most this many events are kept (48 bytes each): beyond it the clock was
// set back months and every entry since then looks recent (see below).
#define PLAYSTATS_EVENTS_MAX  65536

// The index to read the log from for entries since `since` (user clock), in
// the log [start, start + total): back from the newest entry a chunk at a
// time, until a whole chunk is older. `chunk` holds PLAYSTATS_EVENT_CHUNK.
s32 playstats_first_recent_index(PlayEventQuery query, s32 start, s32 total, u64 since, PdmPlayEvent *chunk);

// The log entries since `since`, oldest first, at most `max` of them: the
// newest ones when there are more, and *capped says so. *events is malloc'ed
// (NULL when empty); the caller frees it.
Result playstats_collect_events(PlayEventQuery query, s32 start, s32 total, u64 since, size_t max,
                                PlayLogEvent **events, size_t *count, bool *capped);

// Every game the log [start, start + total) still names (a game in focus),
// once each, in the order first met: also a game deleted before PlayGuard
// first read the console, whose all-time totals pdm still has. At most `max`
// of them; returns how many were written to `ids`. *rc: a failed read (what
// was found before it is kept).
size_t playstats_logged_games(PlayEventQuery query, s32 start, s32 total, u64 *ids, size_t max, Result *rc);

// One game's all-time totals (pdm:qry, for every account or one) into `g`:
// false when they could not be read, or the game was never played.
typedef bool (*PlayTotalsQuery)(void *ctx, u64 app_id, GameStat *g);

// Adds each game of `ids` not listed in `out` yet that `query` finds played,
// while there is room (PLAYSTATS_MAX): the games already there (this week's)
// are never pushed out. One more played game sets out->truncated.
void playstats_add_played(PlayStats *out, const u64 *ids, size_t count, PlayTotalsQuery query, void *ctx);

// The names already read: this run's, and the last run's from the SD cache
// (PlayStats.names_lang says in which console language they are).
typedef struct {
    u8  lang;    // PlayStats.names_lang of every name held, 0 unknown
    u32 count;
    struct { u64 id; char name[PLAYSTATS_NAME_LEN]; } e[PLAYSTATS_MAX];
} PlayNames;

// The console language is `lang` now: names in another one are dropped.
void playnames_use_language(PlayNames *m, u8 lang);
// Adds the names of an earlier read (its language unknown: none).
void playnames_seed(PlayNames *m, const PlayStats *known);
const char *playnames_find(const PlayNames *m, u64 id);   // NULL when unknown
void playnames_add(PlayNames *m, u64 id, const char *name);


#ifdef __cplusplus
}
#endif
