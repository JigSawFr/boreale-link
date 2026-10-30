// bl_notices — the shared bell of the Boréale homebrews:
// sd:/config/boreale/notices/<module>.json, one file per homebrew (or its
// sysmodule), which writes only its own and reads the others'. Each bell
// shows its own homebrew's notices first, then the others', and a click on
// another's opens it at the screen its notice names.
//
// A notice is already written in the console's language (`lang` says which:
// one in another language is not shown until its homebrew writes it again)
// and carries no action: only a route, the screen of its homebrew to open
// (bl_open_args()). Nothing runs from one homebrew in another; the one
// opened asks what it always asks (PlayGuard's PIN).
//
// {"schema":1,"module":"playshelf","nro":"switch/PlayShelf/PlayShelf.nro",
//  "notices":[{"id":"dl-3","level":"progress","title":"Downloading","detail":"Game X, 2 of 3",
//              "lang":"en","progress":40,"updated":1792000000,"expires":0,"route":"downloads/queue"}]}
//
// The rule of the shared files: a `schema` (a file without one is 1), a
// file of a newer schema read as having no notices and never written over,
// and the members a version does not know (of the file and of each notice)
// written back as they were read. No allocation: a sysmodule reads and
// writes them too.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_app.h"
#include "bl_jread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_NOTICES_DIR      BL_LINK_CONF_DIR "/notices"
#define BL_NOTICES_EXT      ".json"
#define BL_NOTICES_SCHEMA   1

#define BL_NOTICES_MAX      16     // notices in a file; more are dropped when read
#define BL_NOTICE_ID_MAX    33
#define BL_NOTICE_TITLE_MAX 97
#define BL_NOTICE_DETAIL_MAX 193
#define BL_NOTICE_LANG_MAX  12     // "en", "zh-Hans"
#define BL_NOTICE_ROUTE_MAX 65     // "downloads/queue"
#define BL_NOTICE_KEPT_MAX  256    // a notice's members this version does not know
#define BL_NOTICES_KEPT_MAX 256    // the file's
// The tokens a file of BL_NOTICES_MAX notices needs, for bl_notices_parse().
#define BL_NOTICES_TOKENS   512

typedef enum {
    BlNotice_Info = 0,   // for information
    BlNotice_Todo,       // waits on the user
    BlNotice_Progress,   // going on (`progress`)
    BlNotice_Error,      // failed
} BlNoticeLevel;

typedef struct {
    char          id[BL_NOTICE_ID_MAX];         // unique in its file
    BlNoticeLevel level;
    char          title[BL_NOTICE_TITLE_MAX];
    char          detail[BL_NOTICE_DETAIL_MAX];
    char          lang[BL_NOTICE_LANG_MAX];     // the language they are written in; "" any
    int8_t        progress;                     // 0..100, -1 none
    int64_t       updated;                      // Unix time
    int64_t       expires;                      // Unix time, 0 never
    char          route[BL_NOTICE_ROUTE_MAX];   // its homebrew's screen; "" its home
    char          kept[BL_NOTICE_KEPT_MAX];     // the members it did not know, as read ("\"a\":1,\"b\":2")
} BlNotice;

typedef struct {
    uint32_t schema;                     // as read; BL_NOTICES_SCHEMA for a new one
    bool     newer;                      // a newer schema: not read, never written over
    char     module[BL_APP_MODULE_MAX];  // whose (bl_app_module_valid)
    char     nro[BL_APP_PATH_MAX];       // its .nro, under the SD card's root: where a click goes
    size_t   count;
    BlNotice items[BL_NOTICES_MAX];
    char     kept[BL_NOTICES_KEPT_MAX];
} BlNotices;

void bl_notices_init(BlNotices *f, const char *module, const char *nro);
// "config/boreale/notices/<module>.json"; false for a module not valid.
bool bl_notices_path(const char *module, char *out, size_t cap);
// The module of a file of the directory ("playshelf.json" -> "playshelf").
bool bl_notices_module_of(const char *file_name, char *out, size_t cap);

// Reads a file over `f` (bl_notices_init first), with `ntoks` tokens of
// the caller's (BL_NOTICES_TOKENS; a sysmodule keeps them static): false
// when it is not one (not JSON, not an object), `f` then empty. A newer
// schema gives true with `newer` and no notices. A notice without an id,
// or of a level this version does not know, is left out.
bool bl_notices_parse(BlNotices *f, const char *text, size_t len, BlJTok *toks, int ntoks);
// Whether `f` may be written: not a newer schema's.
bool bl_notices_may_write(const BlNotices *f);
// The file's content; the length written, 0 when `cap` is too small.
size_t bl_notices_write(const BlNotices *f, char *out, size_t cap);

// Adds `n`, or replaces the notice of the same id (its kept members stay):
// false when the file is full.
bool bl_notices_put(BlNotices *f, const BlNotice *n);
// Removes the notice `id`: false when there was none.
bool bl_notices_remove(BlNotices *f, const char *id);
// Removes the notices expired at `now`: how many.
size_t bl_notices_prune(BlNotices *f, int64_t now);
const BlNotice *bl_notices_find(const BlNotices *f, const char *id);

// Shown at `now` on a console in `lang`: not expired, and in that language
// (or any). Whether its .nro still exists is the caller's to check.
bool bl_notice_shown(const BlNotice *n, int64_t now, const char *lang);
// What the bell's counter counts: the notices shown that are to do or failed.
size_t bl_notices_attention(const BlNotices *f, int64_t now, const char *lang);
// Whether a progress moving from what `written` says to `progress` at `now`
// is worth writing (the SD card spared): every 5 %, every 30 s, and its end.
bool bl_notice_progress_due(const BlNotice *written, int progress, int64_t now);

const char *bl_notice_level_name(BlNoticeLevel level);   // "info", "todo", "progress", "error"
bool bl_notice_level_parse(const char *name, BlNoticeLevel *out);

// A route: lower-case letters, digits, '_', '-' and '/' between names
// ("downloads/queue"), under BL_NOTICE_ROUTE_MAX bytes; "" is the home.
bool bl_route_valid(const char *route);
// The arguments a homebrew is started with to open `route`, for libnx's
// envSetNextLoad(path, args): "sdmc:/<nro> --open=<route>" (the path quoted
// when it has a space); the path alone for "". False when it does not fit or
// the route is not valid. `path` gets "sdmc:/<nro>".
bool bl_open_args(const char *nro, const char *route, char *path, size_t path_cap, char *args, size_t args_cap);
// The route a homebrew was started with ("--open=<route>" among its
// arguments): false when none or not valid.
bool bl_open_route(int argc, char **argv, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
