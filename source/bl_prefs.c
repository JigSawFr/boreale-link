// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_prefs.h"

#include <stdio.h>
#include <string.h>

static const char *const KEYS[BlPref_Count] = { "language", "theme", "text_size", "effects", "density" };

void bl_prefs_defaults(BlPrefs *p)
{
    memset(p, 0, sizeof(*p));
    bl_kv_file_init(&p->file, p->kept, sizeof(p->kept));
    p->file.schema = BL_PREFS_SCHEMA;
}

const char *bl_prefs_key(BlPref pref)
{
    return (unsigned)pref < BlPref_Count ? KEYS[pref] : "";
}

static bool value_ok(const char *v)
{
    if (!v || !*v || strlen(v) >= BL_PREFS_VALUE_MAX) return false;
    for (const char *c = v; *c; c++) {
        const char ch = *c;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.' || ch == ':'))
            return false;
    }
    return true;
}

bool bl_prefs_set(BlPrefs *p, BlPref pref, const char *value)
{
    if ((unsigned)pref >= BlPref_Count) return false;
    if (!value || !*value) {
        p->value[pref][0] = '\0';
        p->set[pref] = false;
        return true;
    }
    if (!value_ok(value)) return false;
    snprintf(p->value[pref], sizeof(p->value[pref]), "%s", value);
    p->set[pref] = true;
    return true;
}

const char *bl_prefs_get(const BlPrefs *p, BlPref pref)
{
    return (unsigned)pref < BlPref_Count && p->set[pref] ? p->value[pref] : NULL;
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlPrefs *p = (BlPrefs *)ctx;
    for (int i = 0; i < BlPref_Count; i++) {
        if (strcmp(key, KEYS[i])) continue;
        if (value_ok(value)) bl_prefs_set(p, (BlPref)i, value);
        return;
    }
    bl_kv_keep(&p->file, key, value);
}

void bl_prefs_parse(BlPrefs *p, const char *text, size_t len)
{
    bl_kv_file_init(&p->file, p->kept, sizeof(p->kept));   // a copied BlPrefs: its own buffer
    bl_kv_read(&p->file, text, len, on_kv, p);
}

bool bl_prefs_may_write(const BlPrefs *p)
{
    return bl_kv_may_rewrite(&p->file, BL_PREFS_SCHEMA);
}

size_t bl_prefs_write(const BlPrefs *p, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        "# Boréale: the look the homebrews share (each one may keep its own).\n"
        "schema=%u\n", (unsigned)BL_PREFS_SCHEMA);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t len = (size_t)n;
    for (int i = 0; i < BlPref_Count; i++) {
        if (!p->set[i]) continue;
        n = snprintf(out + len, cap - len, "%s=%s\n", KEYS[i], p->value[i]);
        if (n < 0 || (size_t)n >= cap - len) return 0;
        len += (size_t)n;
    }
    BlKvFile kept = p->file;
    kept.kept = (char *)p->kept;   // this BlPrefs' lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, len);
}
