// bl_updates — the newest release of each Boréale homebrew installed, as the
// last update check found it, sd:/config/boreale/updates.conf. One homebrew
// checks for all of them (at most once a day: `checked`), from each one's
// releases (bl_app's `repo`); the others read it to show an update without
// asking the network themselves, their own or another homebrew's.
//
//     schema=1
//     checked=1792000000       when the last check ran (Unix time)
//     latest.playguard=1.4.0   a homebrew's newest release (its module: bl_app)
//     latest.playshelf=0.9.2
//
// A bl_kv file with a `schema`: the keys a version does not know are kept
// and written back, a file of a newer schema read but never written over.
// No allocation: a sysmodule may check too.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_app.h"
#include "bl_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_UPDATES_FILE     BL_LINK_CONF_DIR "/updates.conf"   // under the SD card's root
#define BL_UPDATES_SCHEMA   1
#define BL_UPDATES_MAX      16                                 // homebrews kept; more are dropped when read
#define BL_UPDATES_KEPT_MAX 256
#define BL_UPDATES_PREFIX   "latest."                         // the key of a homebrew's line
// How long a check holds: the next one is due a day later.
#define BL_UPDATES_EVERY_S  (24 * 3600)

typedef struct {
    char module[BL_APP_MODULE_MAX];
    char version[BL_APP_VERSION_MAX];
} BlUpdate;

typedef struct {
    int64_t  checked;   // Unix time of the last check, 0 never
    size_t   count;
    BlUpdate items[BL_UPDATES_MAX];
    BlKvFile file;      // its schema, what it skipped, the lines kept
    char     kept[BL_UPDATES_KEPT_MAX];
} BlUpdates;

void bl_updates_defaults(BlUpdates *u);
// A BlUpdates may be copied by value.

// Reads `text` over `u` (bl_updates_defaults first): a module that is not
// valid (bl_app_module_valid) or a version too long or empty is left out.
void bl_updates_parse(BlUpdates *u, const char *text, size_t len);
// The newest release known of `module`, NULL when none.
const char *bl_updates_get(const BlUpdates *u, const char *module);
// Sets the newest release of `module` (replacing what was known): false when
// the module or the version is not valid, or the file is full.
bool bl_updates_set(BlUpdates *u, const char *module, const char *version);
// Whether a check is due at `now`: never checked, a day gone, or a clock set
// back before the last one.
bool bl_updates_due(const BlUpdates *u, int64_t now);

// Whether what was read may be written back (bl_kv_may_rewrite).
bool bl_updates_may_write(const BlUpdates *u);
// The file's content; the length written, 0 when `cap` is too small.
size_t bl_updates_write(const BlUpdates *u, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
