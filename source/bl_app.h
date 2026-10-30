// bl_app — the registry of the Boréale homebrews installed on the console,
// sd:/config/boreale/apps/<module>.conf: one file per homebrew, written by
// itself when it starts (where its .nro is, its name, its version, its icon,
// where its releases are). The others read the directory to know it is
// there: the shared bell's sections, the « Boréale » screen that lists and
// launches them, the update check made for all of them.
// A bl_kv file with a `schema`: the keys a version does not know are kept
// and written back, a file of a newer schema is read but never written over.
//
//     BlApp a;
//     bl_app_defaults(&a);
//     bl_app_path("playguard", path, sizeof(path));
//     if (read_file(path, text, &len)) bl_app_parse(&a, text, len);
//     … set its fields …
//     if (bl_app_may_write(&a) && (n = bl_app_write(&a, out, sizeof(out))) && (n != len || memcmp(out, text, n)))
//         save(path, out, n);   // only when something changed: the SD card is not written at each start
//
// Reading the registry: list BL_APPS_DIR, bl_app_module_of() on each name,
// then read and parse the file.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_kv.h"
#include "bl_link_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

// The directory, under the SD card's root, and its files' extension.
#define BL_APPS_DIR        BL_LINK_CONF_DIR "/apps"
#define BL_APPS_EXT        ".conf"

#define BL_APP_SCHEMA      1

#define BL_APP_MODULE_MAX  33     // the module: its topics' level, "playguard"
#define BL_APP_NAME_MAX    65
#define BL_APP_VERSION_MAX 33
#define BL_APP_PATH_MAX    256
#define BL_APP_REPO_MAX    101    // "owner/name" on GitHub
#define BL_APP_KEPT_MAX    512    // the lines of keys this version does not know

typedef struct {
    char     module[BL_APP_MODULE_MAX];    // from the file's name, not a key
    char     name[BL_APP_NAME_MAX];        // shown: "PlayGuard"
    char     version[BL_APP_VERSION_MAX];  // "1.4.0"
    char     nro[BL_APP_PATH_MAX];         // under the SD card's root: "switch/PlayGuard/PlayGuard.nro"
    char     icon[BL_APP_PATH_MAX];        // a JPEG under the SD card's root, "" the .nro's own
    char     repo[BL_APP_REPO_MAX];        // where its releases are: "JigSawFr/PlayGuard"
    bool     read;                         // the file had at least one line read
    BlKvFile file;                         // its schema, what it skipped, the lines kept
    char     kept[BL_APP_KEPT_MAX];
} BlApp;

void bl_app_defaults(BlApp *a);
// A BlApp may be copied by value.

// Lower-case letters, digits, '_' and '-', under BL_APP_MODULE_MAX bytes: a
// file name and a topic level alike.
bool bl_app_module_valid(const char *module);
// "config/boreale/apps/<module>.conf"; false for a module that is not valid
// or a `cap` too small.
bool bl_app_path(const char *module, char *out, size_t cap);
// The module of a file of the directory ("playguard.conf" -> "playguard");
// false for any other name (another extension, a module not valid).
bool bl_app_module_of(const char *file_name, char *out, size_t cap);

// Reads `text` over what `a` holds (call bl_app_defaults first): a value too
// long keeps what was there, an unknown key is kept for the round trip.
void bl_app_parse(BlApp *a, const char *text, size_t len);
// Whether what was read may be written back (bl_kv_may_rewrite): not a file
// of a newer schema, nor one with a line skipped or kept lines lost.
bool bl_app_may_write(const BlApp *a);
// The file's content, the keys kept after its own; the length written, 0
// when `cap` is too small.
size_t bl_app_write(const BlApp *a, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
