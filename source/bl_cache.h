// bl_cache — the games' icons and names shared by the Boréale homebrews,
// sd:/config/boreale/cache/: what one homebrew read from the console (ns'
// control data) the others find at once, kept once on the SD card.
//
//     config/boreale/cache/cache.conf       schema=1, lang=<n>: the console language they are in
//     config/boreale/cache/icons/<ID>.jpg   a game's icon, its title ID in 16 upper-case hex digits
//     config/boreale/cache/names.txt        schema=1, then <ID>=<name>, one a line
//
// One rule for emptying it, whoever opens it: the console language changed
// (its names and icons are in the old one), or the icons over their bounds
// (BL_CACHE_ICONS_MAX_*: a crash between two runs, or simply many games): the
// icons and the names go, the homebrews read them again as they need them. A
// cache.conf of a newer schema leaves the cache alone (never emptied nor
// written: bl_cache_open() says it is not usable), and the keys this version
// does not know are written back as read.
//
// The files are written aside and renamed (a file cut by a crash is never
// read), readable by all, writable by their owner. No allocation: the agent
// sysmodule reads an icon with bl_cache_icon_path() and its own stream.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_link_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Under the SD card's root.
#define BL_CACHE_DIR        BL_LINK_CONF_DIR "/cache"
#define BL_CACHE_FILE       BL_CACHE_DIR "/cache.conf"
#define BL_CACHE_ICONS_DIR  BL_CACHE_DIR "/icons"
#define BL_CACHE_NAMES_FILE BL_CACHE_DIR "/names.txt"
#define BL_CACHE_SCHEMA     1

// An icon's largest size (NsApplicationControlData's), and the icons' bounds.
#define BL_CACHE_ICON_MAX        0x20000
#define BL_CACHE_ICONS_MAX_BYTES (16u << 20)
#define BL_CACHE_ICONS_MAX_FILES 1024u
// A name's largest size, its '\0' counted (a control.nacp's is 0x200).
#define BL_CACHE_NAME_MAX        0x201

#define BL_CACHE_ROOT_MAX   160
#define BL_CACHE_KEPT_MAX   256    // cache.conf's keys this version does not know

typedef struct {
    char     root[BL_CACHE_ROOT_MAX];   // the SD card's root, as given
    uint8_t  lang;                      // the console language: SetLanguage + 1, 0 unknown
    bool     ready;                     // usable: opened, not of a newer schema
    uint64_t bytes;                     // the icons kept
    uint32_t files;
    char     kept[BL_CACHE_KEPT_MAX];
} BlCache;

// Opens the cache under `root` ("/" or "sdmc:/" on the console, a folder on
// the desktop) for the console language `lang` (not 0): makes its folders,
// empties it when the language changed or the icons are over their bounds,
// and writes cache.conf then. False, `ready` false, when it cannot be used
// (a newer schema, a folder that cannot be made, `lang` 0).
bool bl_cache_open(BlCache *c, const char *root, uint8_t lang);

// `<root>/config/boreale/cache/icons/<ID>.jpg`; false when it does not fit.
bool bl_cache_icon_path(const char *root, uint64_t id, char *out, size_t cap);

// The icon kept for `id` into `buf` (`cap` bytes): its size, 0 when there is
// none, it is not a JPEG or it does not fit.
size_t bl_cache_icon_get(const BlCache *c, uint64_t id, void *buf, size_t cap);
// Keeps `id`'s icon (a JPEG of at most BL_CACHE_ICON_MAX bytes) while the
// icons are under their bounds: false when it was not kept.
bool bl_cache_icon_put(BlCache *c, uint64_t id, const void *jpeg, size_t size);

// The name kept for `id` into `out`: false when there is none.
bool bl_cache_name_get(const BlCache *c, uint64_t id, char *out, size_t cap);
// Keeps the names of `count` games (`ids[i]`, `names[i]`; an empty name or
// one with a line break is left out), replacing those kept for them: one
// rewrite of names.txt for them all. False when it was not written.
bool bl_cache_names_put(BlCache *c, const uint64_t *ids, const char *const *names, size_t count);

#ifdef __cplusplus
}
#endif
