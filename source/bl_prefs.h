// bl_prefs — the look the Boréale homebrews share, sd:/config/boreale/prefs.conf:
// the language, the theme, the text size, the effects and the density. Each
// homebrew follows them by default and may keep a value of its own instead
// (its own settings say which); whichever changes a shared one writes it
// here, and the others take it at their next start.
//
//     schema=1
//     language=fr        "system" or a language code
//     theme=dark         "system", "light" or "dark"
//     text_size=auto
//     effects=auto
//     density=auto
//
// The values are the homebrews' own words (Aurore's Cadran: ui::Prefs); this
// file only carries them, each one under BL_PREFS_VALUE_MAX bytes, and says
// which are set: a key missing has no shared value yet (the homebrew keeps
// its own). A bl_kv file with a `schema`: the keys a version does not know
// are kept and written back, a file of a newer schema read but never written
// over.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bl_kv.h"
#include "bl_link_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_PREFS_FILE      BL_LINK_CONF_DIR "/prefs.conf"   // under the SD card's root
#define BL_PREFS_SCHEMA    1
#define BL_PREFS_VALUE_MAX 24
#define BL_PREFS_KEPT_MAX  512

typedef enum {
    BlPref_Language = 0,
    BlPref_Theme,
    BlPref_TextSize,
    BlPref_Effects,
    BlPref_Density,
    BlPref_Count,
} BlPref;

typedef struct {
    char     value[BlPref_Count][BL_PREFS_VALUE_MAX];   // "" when not set
    bool     set[BlPref_Count];
    BlKvFile file;                                      // its schema, what it skipped, the lines kept
    char     kept[BL_PREFS_KEPT_MAX];
} BlPrefs;

void bl_prefs_defaults(BlPrefs *p);
// A BlPrefs may be copied by value.

// The key of `pref` ("language", "theme", "text_size", "effects", "density").
const char *bl_prefs_key(BlPref pref);

// Reads `text` over `p` (bl_prefs_defaults first): a value too long, empty
// or with a character outside letters, digits, '-', '_', '.' and ':' is
// left unset; an unknown key is kept for the round trip.
void bl_prefs_parse(BlPrefs *p, const char *text, size_t len);
// Sets `pref` to `value` (false, nothing changed, when it is not one this
// file carries); NULL or "" unsets it.
bool bl_prefs_set(BlPrefs *p, BlPref pref, const char *value);
// The value of `pref`, NULL when not set.
const char *bl_prefs_get(const BlPrefs *p, BlPref pref);

// Whether what was read may be written back (bl_kv_may_rewrite): not a file
// of a newer schema, nor one with a line skipped or kept lines lost.
bool bl_prefs_may_write(const BlPrefs *p);
// The file's content, the set keys then the kept ones; the length written,
// 0 when `cap` is too small.
size_t bl_prefs_write(const BlPrefs *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
