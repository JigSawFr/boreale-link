// sync_icons — the game icons' records (docs/sync-protocol.md,
// `icon/<APP_ID>`), shared by PlayGuard (action/sync_icons_logic) and the
// agent sysmodule: which broker and topics an icon went to (a hash, so the
// files do not name the broker), and whether a record lists a game there.
// A record is icons.txt (PlayGuard's) or agent_icons.txt (the agent's):
// "target=<8 hex>", then one "0100000000010000=<bytes>" line per icon.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_ICONS_TARGET_LEN 8

// FNV-1a of "<host>:<port>/<base>" (base: "<prefix>/<id>"), 8 lower-case hex digits.
void sync_icons_target(const char *host, uint16_t port, const char *base, char out[SYNC_ICONS_TARGET_LEN + 1]);

// The record `text` (`len` bytes) is for `target` and lists `app_id`.
bool sync_icons_listed(const char *text, size_t len, const char *target, uint64_t app_id);

// Adds `app_id` (`bytes`) to the record `text` (`len` bytes, NUL-terminated
// in a buffer of `cap`), in place: a record for another target (or none)
// starts again with this one. The new length; 0 when it does not fit.
size_t sync_icons_add(char *text, size_t len, size_t cap, const char *target, uint64_t app_id, uint32_t bytes);

#ifdef __cplusplus
}
#endif
