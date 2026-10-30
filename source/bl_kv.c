// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_kv.h"

#include <string.h>

bool bl_parse_uint(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    if (v < min || v > max) return false;
    *out = (uint32_t)v;
    return true;
}

static bool equal_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

bool bl_parse_bool(const char *s, bool *out)
{
    if (equal_nocase(s, "1") || equal_nocase(s, "true") || equal_nocase(s, "on") || equal_nocase(s, "yes")) {
        *out = true;
        return true;
    }
    if (equal_nocase(s, "0") || equal_nocase(s, "false") || equal_nocase(s, "off") || equal_nocase(s, "no")) {
        *out = false;
        return true;
    }
    return false;
}

size_t bl_kv_each(const char *text, size_t len, BlKvFn fn, void *ctx)
{
    size_t i = 0, skipped = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        size_t a = i, b = end;
        if (b > a && text[b - 1] == '\r') b--;
        // UTF-8 byte-order mark on the first line.
        if (i == 0 && b - a >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
            (unsigned char)text[2] == 0xBF)
            a += 3;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        if (a < b && text[a] != '#') {
            size_t eq = a;
            while (eq < b && text[eq] != '=') eq++;
            if (eq < b) {
                size_t ke = eq;
                while (ke > a && (text[ke - 1] == ' ' || text[ke - 1] == '\t')) ke--;
                char key[BL_KV_KEY_MAX], value[BL_KV_VALUE_MAX];
                const size_t kn = ke - a, vn = b - eq - 1;
                if (kn > 0 && kn < sizeof(key) && vn < sizeof(value)) {
                    memcpy(key, text + a, kn);
                    key[kn] = '\0';
                    memcpy(value, text + eq + 1, vn);
                    value[vn] = '\0';
                    fn(ctx, key, value);
                } else if (kn > 0) {
                    skipped++;
                }
            }
        }
        i = end + 1;
    }
    return skipped;
}

// A `schema=` value: a whole number from 1, spaces around it allowed; 0 when
// it is not one.
static uint32_t parse_schema(const char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    char digits[16];
    size_t n = 0;
    while (value[n] && value[n] != ' ' && value[n] != '\t') {
        if (n + 1 >= sizeof(digits)) return 0;
        digits[n] = value[n];
        n++;
    }
    digits[n] = '\0';
    for (const char *p = value + n; *p; p++)
        if (*p != ' ' && *p != '\t') return 0;
    uint32_t schema = 0;
    return bl_parse_uint(digits, 1, UINT32_MAX, &schema) ? schema : 0;
}

static void on_schema(void *ctx, const char *key, const char *value)
{
    if (!strcmp(key, BL_KV_SCHEMA_KEY)) *(uint32_t *)ctx = parse_schema(value);
}

uint32_t bl_kv_schema(const char *text, size_t len)
{
    uint32_t schema = BL_KV_SCHEMA_ABSENT;
    bl_kv_each(text, len, on_schema, &schema);
    return schema;
}

void bl_kv_file_init(BlKvFile *f, char *kept, size_t cap)
{
    memset(f, 0, sizeof(*f));
    f->schema = BL_KV_SCHEMA_ABSENT;
    f->kept = kept;
    f->cap = cap;
    if (kept && cap) kept[0] = '\0';
}

typedef struct {
    BlKvFile *f;
    BlKvFn    fn;
    void     *ctx;
} ReadCtx;

static void on_read(void *vctx, const char *key, const char *value)
{
    ReadCtx *r = (ReadCtx *)vctx;
    if (!strcmp(key, BL_KV_SCHEMA_KEY))
        r->f->schema = parse_schema(value);
    else
        r->fn(r->ctx, key, value);
}

void bl_kv_read(BlKvFile *f, const char *text, size_t len, BlKvFn fn, void *ctx)
{
    ReadCtx r = { f, fn, ctx };
    f->schema = BL_KV_SCHEMA_ABSENT;
    f->len = 0;
    f->overflow = false;
    if (f->kept && f->cap) f->kept[0] = '\0';
    f->skipped = bl_kv_each(text, len, on_read, &r);
}

void bl_kv_keep(BlKvFile *f, const char *key, const char *value)
{
    const size_t kn = strlen(key), vn = strlen(value);
    // "key=value\n" and the '\0' after it
    if (!f->kept || f->len + kn + vn + 3 > f->cap) {
        f->overflow = true;
        return;
    }
    char *p = f->kept + f->len;
    memcpy(p, key, kn);
    p[kn] = '=';
    memcpy(p + kn + 1, value, vn);
    p[kn + 1 + vn] = '\n';
    p[kn + 2 + vn] = '\0';
    f->len += kn + vn + 2;
}

bool bl_kv_may_rewrite(const BlKvFile *f, uint32_t mine)
{
    return f->schema != 0 && f->schema <= mine && f->skipped == 0 && !f->overflow;
}

size_t bl_kv_write_kept(const BlKvFile *f, char *out, size_t cap, size_t at)
{
    if (at >= cap || f->len >= cap - at) return 0;
    if (f->len) memcpy(out + at, f->kept, f->len);
    out[at + f->len] = '\0';
    return at + f->len;
}
