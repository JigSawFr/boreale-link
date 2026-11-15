// sync_titles — what a title the play log names is: a game, a homebrew (an
// NRO forwarder on the HOME menu, PlayGuard's own among them) or one of the
// console's own programs. Only games go to the broker by their ID: a
// homebrew's time is told as one total, and "now playing" as a homebrew
// with no ID (docs/sync-protocol.md). Shared by PlayGuard and the agent
// sysmodule, which cannot read a title's control data: PlayGuard writes what
// it knows in nand/<system>/sync/titles.txt, the agent reads it.
//
// From the ID alone (what holds when nothing else is known): Nintendo's
// applications are 0x0100000000010000 and above with 0x01 as their top byte,
// its own programs below 0x0100000000010000; the forwarders sphaira and
// PlayGuard make take 0x05 as their top byte (core/forwarder_pack.h), and any
// ID outside 0x01 is no Nintendo application. A homebrew installed under an
// ID of the 0x01 range is a game by its ID: only the record can say.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_TITLES_MAX 512

// titles.txt as read: one "<16 hex digits>=<kind>[:<name>]" line per title
// that is not a game ("homebrew", "playguard", "system"; "game" for one the
// ID would misplace; a kind this version does not know is read as
// "homebrew": never published by its ID), "#" lines and anything else ignored.
typedef struct {
    size_t   n;
    uint64_t ids[SYNC_TITLES_MAX];
    uint8_t  kinds[SYNC_TITLES_MAX];
} SyncTitles;

SyncTitleKind sync_title_kind_of_id(uint64_t id);
void          sync_titles_parse(SyncTitles *t, const char *text, size_t len);
// What the record says of `id`, else its ID. `t` may be NULL (the ID alone).
SyncTitleKind sync_titles_kind(const SyncTitles *t, uint64_t id);
// "game", "homebrew", "system", "playguard" (titles.txt's words).
const char   *sync_title_kind_name(SyncTitleKind k);
// What the documents call it: the same, PlayGuard's own forwarder "homebrew".
const char   *sync_title_kind_label(SyncTitleKind k);
// activity_today.now: "game", "homebrew", "playguard", "home", "suspended";
// NULL unknown.
const char   *sync_now_name(uint64_t playing, SyncTitleKind kind, SyncNow now);
// Keeps the games of `apps` in place, in their order, and returns how many;
// the others' seconds are added to *other_s.
size_t        sync_titles_games_only(const SyncTitles *t, SyncAppTime *apps, size_t n, uint32_t *other_s);

#ifdef __cplusplus
}
#endif
