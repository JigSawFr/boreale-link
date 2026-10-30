// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_app.h"

#include <stdio.h>
#include <string.h>

void bl_app_defaults(BlApp *a)
{
    memset(a, 0, sizeof(*a));
    bl_kv_file_init(&a->file, a->kept, sizeof(a->kept));
    a->file.schema = BL_APP_SCHEMA;
}

static bool module_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

bool bl_app_module_valid(const char *module)
{
    if (!module || !*module || strlen(module) >= BL_APP_MODULE_MAX) return false;
    for (const char *p = module; *p; p++)
        if (!module_char(*p)) return false;
    return true;
}

bool bl_app_path(const char *module, char *out, size_t cap)
{
    if (!bl_app_module_valid(module)) return false;
    const int n = snprintf(out, cap, "%s/%s%s", BL_APPS_DIR, module, BL_APPS_EXT);
    return n > 0 && (size_t)n < cap;
}

bool bl_app_module_of(const char *file_name, char *out, size_t cap)
{
    const size_t ext = sizeof(BL_APPS_EXT) - 1;
    const size_t len = file_name ? strlen(file_name) : 0;
    if (len <= ext || strcmp(file_name + len - ext, BL_APPS_EXT)) return false;
    char module[BL_APP_MODULE_MAX];
    if (len - ext >= sizeof(module)) return false;
    memcpy(module, file_name, len - ext);
    module[len - ext] = '\0';
    if (!bl_app_module_valid(module) || strlen(module) >= cap) return false;
    snprintf(out, cap, "%s", module);
    return true;
}

static void str_field(char *dst, size_t cap, const char *value)
{
    if (strlen(value) < cap) snprintf(dst, cap, "%s", value);
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlApp *a = (BlApp *)ctx;
    a->read = true;
    if (!strcmp(key, "name")) str_field(a->name, sizeof(a->name), value);
    else if (!strcmp(key, "version")) str_field(a->version, sizeof(a->version), value);
    else if (!strcmp(key, "nro")) str_field(a->nro, sizeof(a->nro), value);
    else if (!strcmp(key, "icon")) str_field(a->icon, sizeof(a->icon), value);
    else if (!strcmp(key, "repo")) str_field(a->repo, sizeof(a->repo), value);
    else bl_kv_keep(&a->file, key, value);
}

void bl_app_parse(BlApp *a, const char *text, size_t len)
{
    bl_kv_file_init(&a->file, a->kept, sizeof(a->kept));   // a copied BlApp: its own buffer
    bl_kv_read(&a->file, text, len, on_kv, a);
}

bool bl_app_may_write(const BlApp *a)
{
    return bl_kv_may_rewrite(&a->file, BL_APP_SCHEMA);
}

size_t bl_app_write(const BlApp *a, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
        "# Boréale: a homebrew installed on this console, written by itself when it\n"
        "# starts so that the others know it is there.\n"
        "schema=%u\n"
        "name=%s\n"
        "version=%s\n"
        "nro=%s\n"
        "icon=%s\n"
        "repo=%s\n",
        (unsigned)BL_APP_SCHEMA, a->name, a->version, a->nro, a->icon, a->repo);
    if (n < 0 || (size_t)n >= cap) return 0;
    BlKvFile kept = a->file;
    kept.kept = (char *)a->kept;   // this BlApp's lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, (size_t)n);
}
