// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_updates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bl_updates_defaults(BlUpdates *u)
{
    memset(u, 0, sizeof(*u));
    bl_kv_file_init(&u->file, u->kept, sizeof(u->kept));
    u->file.schema = BL_UPDATES_SCHEMA;
}

static bool version_ok(const char *v)
{
    if (!v || !*v || strlen(v) >= BL_APP_VERSION_MAX) return false;
    for (const char *c = v; *c; c++)
        if ((unsigned char)*c <= ' ' || *c == '=' || *c == '#') return false;
    return true;
}

static BlUpdate *find(BlUpdates *u, const char *module)
{
    for (size_t i = 0; i < u->count; i++)
        if (!strcmp(u->items[i].module, module)) return &u->items[i];
    return NULL;
}

bool bl_updates_set(BlUpdates *u, const char *module, const char *version)
{
    if (!bl_app_module_valid(module) || !version_ok(version)) return false;
    BlUpdate *it = find(u, module);
    if (!it) {
        if (u->count >= BL_UPDATES_MAX) return false;
        it = &u->items[u->count++];
        snprintf(it->module, sizeof(it->module), "%s", module);
    }
    snprintf(it->version, sizeof(it->version), "%s", version);
    return true;
}

const char *bl_updates_get(const BlUpdates *u, const char *module)
{
    if (!module) return NULL;
    for (size_t i = 0; i < u->count; i++)
        if (!strcmp(u->items[i].module, module)) return u->items[i].version;
    return NULL;
}

bool bl_updates_due(const BlUpdates *u, int64_t now)
{
    return u->checked <= 0 || now < u->checked || now - u->checked >= BL_UPDATES_EVERY_S;
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlUpdates *u = (BlUpdates *)ctx;
    if (!strcmp(key, "checked")) {
        char *end = NULL;
        const long long v = strtoll(value, &end, 10);
        if (end && end != value && *end == '\0' && v > 0) u->checked = v;
        return;
    }
    const size_t pre = sizeof(BL_UPDATES_PREFIX) - 1;
    if (!strncmp(key, BL_UPDATES_PREFIX, pre)) {
        const char *module = key + pre;
        if (!bl_updates_set(u, module, value) && bl_app_module_valid(module) && version_ok(value) &&
            u->count >= BL_UPDATES_MAX)
            bl_kv_keep(&u->file, key, value);   // past the bound: kept as read, not lost
        return;   // not valid: left out
    }
    bl_kv_keep(&u->file, key, value);
}

void bl_updates_parse(BlUpdates *u, const char *text, size_t len)
{
    bl_kv_file_init(&u->file, u->kept, sizeof(u->kept));   // a copied BlUpdates: its own buffer
    bl_kv_read(&u->file, text, len, on_kv, u);
}

bool bl_updates_may_write(const BlUpdates *u)
{
    return bl_kv_may_rewrite(&u->file, BL_UPDATES_SCHEMA);
}

size_t bl_updates_write(const BlUpdates *u, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        "# Boréale: the newest release of each homebrew, as the last update check found it.\n"
        "schema=%u\nchecked=%lld\n", (unsigned)BL_UPDATES_SCHEMA, (long long)u->checked);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t len = (size_t)n;
    for (size_t i = 0; i < u->count; i++) {
        n = snprintf(out + len, cap - len, "%s%s=%s\n", BL_UPDATES_PREFIX, u->items[i].module, u->items[i].version);
        if (n < 0 || (size_t)n >= cap - len) return 0;
        len += (size_t)n;
    }
    BlKvFile kept = u->file;
    kept.kept = (char *)u->kept;   // this BlUpdates' lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, len);
}
