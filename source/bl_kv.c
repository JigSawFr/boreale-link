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

void bl_kv_each(const char *text, size_t len, BlKvFn fn, void *ctx)
{
    size_t i = 0;
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
                char key[48], value[512];
                const size_t kn = ke - a, vn = b - eq - 1;
                if (kn > 0 && kn < sizeof(key) && vn < sizeof(value)) {
                    memcpy(key, text + a, kn);
                    key[kn] = '\0';
                    memcpy(value, text + eq + 1, vn);
                    value[vn] = '\0';
                    fn(ctx, key, value);
                }
            }
        }
        i = end + 1;
    }
}
