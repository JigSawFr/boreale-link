// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_notices.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bl_app.h"
#include "bl_json.h"
#include "bl_notices.h"

#define SYNC_NOTICES_HOMEBREWS 8         // homebrews in the document, the first by file name
#define SYNC_NOTICES_FILE_MAX  (24 * 1024)

static BlNotices s_file;
static BlJTok    s_toks[BL_NOTICES_TOKENS];
static char      s_text[SYNC_NOTICES_FILE_MAX];

// `sd_root` + `rel` into `out`; false when it does not fit.
static bool under(const char *sd_root, const char *rel, char *out, size_t cap)
{
    const size_t n = strlen(sd_root);
    const char *sep = n && sd_root[n - 1] == '/' ? "" : "/";
    const int w = snprintf(out, cap, "%s%s%s", sd_root, sep, rel);
    return w > 0 && (size_t)w < cap;
}

// The whole file into s_text, '\0'-ended: its length, 0 when missing, empty
// or too large to be one of ours.
static size_t read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(s_text, 1, sizeof(s_text) - 1, f);
    const bool more = fgetc(f) != EOF;
    fclose(f);
    if (more) return 0;
    s_text[n] = '\0';
    return n;
}

// The modules with a file in the folder, sorted: PlayGuard's comes like any other.
static size_t modules(const char *dir, char (*out)[BL_APP_MODULE_MAX], size_t max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char m[BL_APP_MODULE_MAX];
        if (!bl_notices_module_of(ent->d_name, m, sizeof(m))) continue;
        // Insertion sort, the first `max` by name.
        size_t at = n;
        while (at > 0 && strcmp(out[at - 1], m) > 0) at--;
        if (at >= max) continue;
        if (n < max) n++;
        memmove(out[at + 1], out[at], (n - 1 - at) * sizeof(out[0]));
        snprintf(out[at], sizeof(out[0]), "%s", m);
    }
    closedir(d);
    return n;
}

size_t sync_notices_build(const char *sd_root, int64_t now, char *out, size_t cap)
{
    char dir[BL_APP_PATH_MAX + 32], path[BL_APP_PATH_MAX + 32];
    char found[SYNC_NOTICES_HOMEBREWS][BL_APP_MODULE_MAX];
    const size_t n = under(sd_root, BL_NOTICES_DIR, dir, sizeof(dir)) ? modules(dir, found, SYNC_NOTICES_HOMEBREWS) : 0;
    unsigned todo = 0, failed = 0;
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_NOTICES_SCHEMA);
    bl_json_key(&j, "homebrews");
    bl_json_arr(&j);
    for (size_t i = 0; i < n; i++) {
        char rel[BL_APP_PATH_MAX];
        if (!bl_notices_path(found[i], rel, sizeof(rel)) || !under(sd_root, rel, path, sizeof(path))) continue;
        const size_t len = read_text(path);
        bl_notices_init(&s_file, found[i], "");
        if (!len || !bl_notices_parse(&s_file, s_text, len, s_toks, BL_NOTICES_TOKENS) || s_file.newer) continue;
        // Its .nro gone (the homebrew deleted): its file is left over. One
        // started from elsewhere (nxlink) does not know its .nro: kept.
        struct stat st;
        if (s_file.nro[0] && (!under(sd_root, s_file.nro[0] == '/' ? s_file.nro + 1 : s_file.nro, path, sizeof(path)) ||
                              stat(path, &st) != 0))
            continue;
        // Its name: the registry's, else its module.
        char name[BL_APP_NAME_MAX];
        snprintf(name, sizeof(name), "%.*s", (int)sizeof(found[0]) - 1, found[i]);
        BlApp app;
        bl_app_defaults(&app);
        if (bl_app_path(found[i], rel, sizeof(rel)) && under(sd_root, rel, path, sizeof(path))) {
            const size_t app_len = read_text(path);
            if (app_len) bl_app_parse(&app, s_text, app_len);
            if (app.name[0]) snprintf(name, sizeof(name), "%s", app.name);
        }
        bl_json_obj(&j);
        bl_json_kstr(&j, "module", found[i]);
        bl_json_kstr(&j, "name", name);
        bl_json_key(&j, "notices");
        bl_json_arr(&j);
        for (size_t k = 0; k < s_file.count; k++) {
            const BlNotice *e = &s_file.items[k];
            if (e->expires && now >= e->expires) continue;
            todo += e->level == BlNotice_Todo;
            failed += e->level == BlNotice_Error;
            bl_json_obj(&j);
            bl_json_kstr(&j, "id", e->id);
            bl_json_kstr(&j, "level", bl_notice_level_name(e->level));
            bl_json_kstr(&j, "title", e->title);
            bl_json_kstr(&j, "detail", e->detail);
            bl_json_kstr(&j, "lang", e->lang);
            if (e->progress >= 0) bl_json_kint(&j, "progress", e->progress);
            bl_json_kint(&j, "updated", e->updated);
            bl_json_kstr(&j, "route", e->route);
            bl_json_obj_end(&j);
        }
        bl_json_arr_end(&j);
        bl_json_obj_end(&j);
    }
    bl_json_arr_end(&j);
    bl_json_kint(&j, "todo", todo);
    bl_json_kint(&j, "error", failed);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}
