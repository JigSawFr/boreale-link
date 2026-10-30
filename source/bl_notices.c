// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_notices.h"

#include <stdio.h>
#include <string.h>

#include "bl_json.h"

static const char *const LEVELS[] = {"info", "todo", "progress", "error"};

const char *bl_notice_level_name(BlNoticeLevel level)
{
    return (unsigned)level < sizeof(LEVELS) / sizeof(LEVELS[0]) ? LEVELS[level] : "info";
}

bool bl_notice_level_parse(const char *name, BlNoticeLevel *out)
{
    for (unsigned i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++)
        if (!strcmp(name, LEVELS[i])) {
            *out = (BlNoticeLevel)i;
            return true;
        }
    return false;
}

void bl_notices_init(BlNotices *f, const char *module, const char *nro)
{
    memset(f, 0, sizeof(*f));
    f->schema = BL_NOTICES_SCHEMA;
    if (module && bl_app_module_valid(module)) snprintf(f->module, sizeof(f->module), "%s", module);
    if (nro && strlen(nro) < sizeof(f->nro)) snprintf(f->nro, sizeof(f->nro), "%s", nro);
}

bool bl_notices_path(const char *module, char *out, size_t cap)
{
    if (!bl_app_module_valid(module)) return false;
    const int n = snprintf(out, cap, "%s/%s%s", BL_NOTICES_DIR, module, BL_NOTICES_EXT);
    return n > 0 && (size_t)n < cap;
}

bool bl_notices_module_of(const char *file_name, char *out, size_t cap)
{
    const size_t ext = sizeof(BL_NOTICES_EXT) - 1;
    const size_t len = file_name ? strlen(file_name) : 0;
    char module[BL_APP_MODULE_MAX];
    if (len <= ext || strcmp(file_name + len - ext, BL_NOTICES_EXT) || len - ext >= sizeof(module)) return false;
    memcpy(module, file_name, len - ext);
    module[len - ext] = '\0';
    if (!bl_app_module_valid(module) || strlen(module) >= cap) return false;
    snprintf(out, cap, "%s", module);
    return true;
}

// Appends a member as read to `kept` ("a":1,"b":2); a member that does not
// fit is lost.
static void keep(char *kept, size_t cap, const char *text, const BlJTok *t, int count, int key)
{
    const size_t len = strlen(kept);
    const size_t sep = len ? 1 : 0;
    if (len + sep + 1 >= cap) return;
    const size_t n = bl_jread_member(text, t, count, key, kept + len + sep, cap - len - sep);
    if (n && sep) kept[len] = ',';
}

static bool read_notice(BlNotice *n, const char *text, const BlJTok *t, int count, int obj)
{
    memset(n, 0, sizeof(*n));
    n->progress = -1;
    bool has_id = false, has_level = false;
    for (int i = obj + 1, k = 0; k < t[obj].size; k++) {
        const int key = i, val = i + 1;
        const BlJTok *v = &t[val];
        char word[16];
        int64_t num = 0;
        if (bl_jread_eq(text, &t[key], "id")) {
            has_id = bl_jread_str(text, v, n->id, sizeof(n->id)) && n->id[0];
        } else if (bl_jread_eq(text, &t[key], "level")) {
            has_level = bl_jread_str(text, v, word, sizeof(word)) && bl_notice_level_parse(word, &n->level);
        } else if (bl_jread_eq(text, &t[key], "title")) {
            bl_jread_str(text, v, n->title, sizeof(n->title));
        } else if (bl_jread_eq(text, &t[key], "detail")) {
            bl_jread_str(text, v, n->detail, sizeof(n->detail));
        } else if (bl_jread_eq(text, &t[key], "lang")) {
            bl_jread_str(text, v, n->lang, sizeof(n->lang));
        } else if (bl_jread_eq(text, &t[key], "progress")) {
            if (bl_jread_int(text, v, &num) && num >= 0 && num <= 100) n->progress = (int8_t)num;
        } else if (bl_jread_eq(text, &t[key], "updated")) {
            if (bl_jread_int(text, v, &num) && num >= 0) n->updated = num;
        } else if (bl_jread_eq(text, &t[key], "expires")) {
            if (bl_jread_int(text, v, &num) && num >= 0) n->expires = num;
        } else if (bl_jread_eq(text, &t[key], "route")) {
            char route[BL_NOTICE_ROUTE_MAX];
            if (bl_jread_str(text, v, route, sizeof(route)) && bl_route_valid(route))
                snprintf(n->route, sizeof(n->route), "%s", route);
        } else {
            keep(n->kept, sizeof(n->kept), text, t, count, key);
        }
        i = bl_jread_skip(t, count, val);
    }
    return has_id && has_level;
}

bool bl_notices_parse(BlNotices *f, const char *text, size_t len, BlJTok *t, int ntoks)
{
    char module[BL_APP_MODULE_MAX], nro[BL_APP_PATH_MAX];
    snprintf(module, sizeof(module), "%s", f->module);
    snprintf(nro, sizeof(nro), "%s", f->nro);
    bl_notices_init(f, module, nro);
    const int count = bl_jread_parse(text, len, t, ntoks);
    if (count < 1 || t[0].type != BlJ_Obj) return false;
    // The schema first: a newer file is not read any further.
    for (int i = 1, k = 0; k < t[0].size; k++) {
        int64_t schema = 0;
        if (bl_jread_eq(text, &t[i], "schema")) {
            if (!bl_jread_int(text, &t[i + 1], &schema) || schema < 1 || schema > UINT32_MAX) schema = UINT32_MAX;
            f->schema = (uint32_t)schema;
        }
        i = bl_jread_skip(t, count, i + 1);
    }
    if (f->schema > BL_NOTICES_SCHEMA) {
        f->newer = true;
        return true;
    }
    for (int i = 1, k = 0; k < t[0].size; k++) {
        const int key = i, val = i + 1;
        if (bl_jread_eq(text, &t[key], "schema")) {
            // read above
        } else if (bl_jread_eq(text, &t[key], "module")) {
            char m[BL_APP_MODULE_MAX];
            if (bl_jread_str(text, &t[val], m, sizeof(m)) && bl_app_module_valid(m))
                snprintf(f->module, sizeof(f->module), "%s", m);
        } else if (bl_jread_eq(text, &t[key], "nro")) {
            bl_jread_str(text, &t[val], f->nro, sizeof(f->nro));
        } else if (bl_jread_eq(text, &t[key], "notices") && t[val].type == BlJ_Arr) {
            for (int j = val + 1, e = 0; e < t[val].size; e++) {
                if (t[j].type == BlJ_Obj && f->count < BL_NOTICES_MAX &&
                    read_notice(&f->items[f->count], text, t, count, j) && !bl_notices_find(f, f->items[f->count].id))
                    f->count++;
                j = bl_jread_skip(t, count, j);
            }
        } else {
            keep(f->kept, sizeof(f->kept), text, t, count, key);
        }
        i = bl_jread_skip(t, count, val);
    }
    return true;
}

bool bl_notices_may_write(const BlNotices *f)
{
    return !f->newer && f->schema <= BL_NOTICES_SCHEMA;
}

size_t bl_notices_write(const BlNotices *f, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", BL_NOTICES_SCHEMA);
    bl_json_kstr(&j, "module", f->module);
    bl_json_kstr(&j, "nro", f->nro);
    bl_json_key(&j, "notices");
    bl_json_arr(&j);
    for (size_t i = 0; i < f->count; i++) {
        const BlNotice *n = &f->items[i];
        bl_json_obj(&j);
        bl_json_kstr(&j, "id", n->id);
        bl_json_kstr(&j, "level", bl_notice_level_name(n->level));
        bl_json_kstr(&j, "title", n->title);
        bl_json_kstr(&j, "detail", n->detail);
        bl_json_kstr(&j, "lang", n->lang);
        if (n->progress >= 0) bl_json_kint(&j, "progress", n->progress);
        bl_json_kint(&j, "updated", n->updated);
        bl_json_kint(&j, "expires", n->expires);
        bl_json_kstr(&j, "route", n->route);
        bl_json_members(&j, n->kept);
        bl_json_obj_end(&j);
    }
    bl_json_arr_end(&j);
    bl_json_members(&j, f->kept);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}

const BlNotice *bl_notices_find(const BlNotices *f, const char *id)
{
    for (size_t i = 0; i < f->count; i++)
        if (!strcmp(f->items[i].id, id)) return &f->items[i];
    return NULL;
}

bool bl_notices_put(BlNotices *f, const BlNotice *n)
{
    if (!n->id[0]) return false;
    BlNotice *at = (BlNotice *)bl_notices_find(f, n->id);
    if (at) {
        char kept[BL_NOTICE_KEPT_MAX];
        memcpy(kept, at->kept, sizeof(kept));
        *at = *n;
        if (!n->kept[0]) memcpy(at->kept, kept, sizeof(kept));
        return true;
    }
    if (f->count >= BL_NOTICES_MAX) return false;
    f->items[f->count++] = *n;
    return true;
}

bool bl_notices_remove(BlNotices *f, const char *id)
{
    for (size_t i = 0; i < f->count; i++)
        if (!strcmp(f->items[i].id, id)) {
            memmove(&f->items[i], &f->items[i + 1], (f->count - i - 1) * sizeof(f->items[0]));
            f->count--;
            return true;
        }
    return false;
}

size_t bl_notices_prune(BlNotices *f, int64_t now)
{
    size_t removed = 0;
    for (size_t i = 0; i < f->count;) {
        if (f->items[i].expires && now >= f->items[i].expires) {
            bl_notices_remove(f, f->items[i].id);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

bool bl_notice_shown(const BlNotice *n, int64_t now, const char *lang)
{
    if (n->expires && now >= n->expires) return false;
    return !n->lang[0] || !lang || !strcmp(n->lang, lang);
}

size_t bl_notices_attention(const BlNotices *f, int64_t now, const char *lang)
{
    size_t count = 0;
    for (size_t i = 0; i < f->count; i++) {
        const BlNotice *n = &f->items[i];
        if ((n->level == BlNotice_Todo || n->level == BlNotice_Error) && bl_notice_shown(n, now, lang)) count++;
    }
    return count;
}

bool bl_notice_progress_due(const BlNotice *written, int progress, int64_t now)
{
    if (!written || written->progress < 0 || progress >= 100) return true;
    const int moved = progress > written->progress ? progress - written->progress : written->progress - progress;
    return moved >= 5 || now - written->updated >= 30;
}

bool bl_route_valid(const char *route)
{
    if (!route) return false;
    const size_t len = strlen(route);
    if (len >= BL_NOTICE_ROUTE_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        const char c = route[i];
        if (c == '/') {
            if (i == 0 || i + 1 == len || route[i - 1] == '/') return false;   // names between the slashes
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool bl_open_args(const char *nro, const char *route, char *path, size_t path_cap, char *args, size_t args_cap)
{
    if (!nro || !*nro || !bl_route_valid(route)) return false;
    if (nro[0] == '/') nro++;
    int n = snprintf(path, path_cap, "sdmc:/%s", nro);
    if (n < 0 || (size_t)n >= path_cap) return false;
    const char *q = strchr(path, ' ') ? "\"" : "";
    n = *route ? snprintf(args, args_cap, "%s%s%s --open=%s", q, path, q, route)
               : snprintf(args, args_cap, "%s%s%s", q, path, q);
    return n > 0 && (size_t)n < args_cap;
}

bool bl_open_route(int argc, char **argv, char *out, size_t cap)
{
    static const char opt[] = "--open=";
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || strncmp(argv[i], opt, sizeof(opt) - 1)) continue;
        const char *route = argv[i] + sizeof(opt) - 1;
        if (!bl_route_valid(route) || strlen(route) >= cap) return false;
        snprintf(out, cap, "%s", route);
        return true;
    }
    return false;
}
