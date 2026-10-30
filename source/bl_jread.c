// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_jread.h"

#include <string.h>

static BlJTok *take(BlJTok *toks, int n, int *count, BlJType type, int start, int end, int parent)
{
    if (*count >= n) return NULL;
    BlJTok *t = &toks[(*count)++];
    t->type = type;
    t->start = start;
    t->end = end;
    t->size = 0;
    t->parent = parent;
    if (parent >= 0) toks[parent].size++;
    return t;
}

// true, false, null or a number as JSON writes it.
static bool prim_ok(const char *s, size_t n)
{
    if ((n == 4 && (!memcmp(s, "true", 4) || !memcmp(s, "null", 4))) || (n == 5 && !memcmp(s, "false", 5)))
        return true;
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i >= n) return false;
    if (s[i] == '0') i++;
    else if (s[i] >= '1' && s[i] <= '9') while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    else return false;
    if (i < n && s[i] == '.') {
        if (++i >= n || s[i] < '0' || s[i] > '9') return false;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        if (++i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= n || s[i] < '0' || s[i] > '9') return false;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    return i == n;
}

int bl_jread_parse(const char *text, size_t len, BlJTok *toks, int n)
{
    if (!text || !toks || n < 1 || len > 0x7fffffff) return -1;
    // `parent` is the open object or array, or the key whose value comes;
    // `want_value` is true where a value (or, in an object, a key) may come.
    int count = 0, parent = -1;
    bool want_value = true;
    for (size_t pos = 0; pos < len; pos++) {
        const char c = text[pos];
        const bool in_obj = parent >= 0 && toks[parent].type == BlJ_Obj;   // a key comes next, not a value
        switch (c) {
            case ' ': case '\t': case '\r': case '\n':
                break;
            case '{': case '[':
                if (!want_value || in_obj) return -1;
                if (!take(toks, n, &count, c == '{' ? BlJ_Obj : BlJ_Arr, (int)pos, -1, parent)) return -2;
                parent = count - 1;
                break;
            case '}': case ']': {
                if (parent < 0) return -1;
                if (toks[parent].type == BlJ_Str) {   // the last key: its value is there
                    if (want_value) return -1;
                    parent = toks[parent].parent;
                } else if (toks[parent].type == BlJ_Obj) {
                    if (toks[parent].size > 0) return -1;   // a key without its value, or a trailing comma
                } else if (want_value && toks[parent].size > 0) {
                    return -1;                              // a trailing comma
                }
                if (toks[parent].type != (c == '}' ? BlJ_Obj : BlJ_Arr)) return -1;
                toks[parent].end = (int)pos + 1;
                parent = toks[parent].parent;
                want_value = false;
                break;
            }
            case '"': {
                if (!want_value) return -1;
                const size_t start = pos + 1;
                for (pos = start; pos < len && text[pos] != '"'; pos++) {
                    if ((unsigned char)text[pos] < 0x20) return -1;
                    if (text[pos] == '\\' && ++pos >= len) return -1;
                }
                if (pos >= len) return -1;
                if (!take(toks, n, &count, BlJ_Str, (int)start, (int)pos, parent)) return -2;
                want_value = false;
                break;
            }
            case ':':
                if (!in_obj || want_value || toks[count - 1].parent != parent || toks[count - 1].type != BlJ_Str) return -1;
                parent = count - 1;
                want_value = true;
                break;
            case ',':
                if (want_value || parent < 0 || in_obj) return -1;
                if (toks[parent].type == BlJ_Str) parent = toks[parent].parent;
                want_value = true;
                break;
            default: {
                if (!want_value || in_obj) return -1;
                const size_t start = pos;
                while (pos < len && strchr("-+.0123456789eEtruefalsn", text[pos])) pos++;
                if (!prim_ok(text + start, pos - start)) return -1;
                if (!take(toks, n, &count, BlJ_Prim, (int)start, (int)pos, parent)) return -2;
                pos--;
                want_value = false;
                break;
            }
        }
    }
    return parent == -1 && count > 0 && !want_value ? count : -1;
}

int bl_jread_skip(const BlJTok *toks, int count, int i)
{
    if (i < 0 || i >= count) return count;
    int j = i + 1;
    while (j < count) {
        // In i when one of its parents is i.
        int p = toks[j].parent;
        while (p > i) p = toks[p].parent;
        if (p != i) break;
        j++;
    }
    return j;
}

bool bl_jread_eq(const char *text, const BlJTok *t, const char *s)
{
    const size_t n = strlen(s);
    return t->type == BlJ_Str && (size_t)(t->end - t->start) == n && !memcmp(text + t->start, s, n);
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(const char *s, int left, uint32_t *out)
{
    if (left < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        const int h = hex(s[k]);
        if (h < 0) return false;
        v = v << 4 | (uint32_t)h;
    }
    *out = v;
    return true;
}

// One character's bytes into `out`: false when they do not fit (the string
// is then cut before it).
static bool put_utf8(char *out, size_t cap, size_t *len, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3F)); b[2] = (char)(0x80 | (cp >> 6 & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*len + n >= cap) return false;
    memcpy(out + *len, b, n);
    *len += n;
    return true;
}

bool bl_jread_str(const char *text, const BlJTok *t, char *out, size_t cap)
{
    if (t->type != BlJ_Str || !cap) return false;
    size_t len = 0;
    bool full = false;
    for (int i = t->start; i < t->end; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c != '\\') {
            // A whole UTF-8 character at a time, so a cut never splits one.
            int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (i + n > t->end) n = t->end - i;
            if (!full && len + (size_t)n < cap) {
                memcpy(out + len, text + i, (size_t)n);
                len += (size_t)n;
            } else {
                full = true;
            }
            i += n - 1;
            continue;
        }
        if (++i >= t->end) return false;
        uint32_t cp;
        switch (text[i]) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
                if (!hex4(text + i + 1, t->end - i - 1, &cp)) return false;
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00) {   // a pair: its second half follows
                    uint32_t lo;
                    if (i + 2 >= t->end || text[i + 1] != '\\' || text[i + 2] != 'u') return false;
                    if (!hex4(text + i + 3, t->end - i - 3, &lo) || lo < 0xDC00 || lo >= 0xE000) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return false;
                }
                break;
            default: return false;
        }
        if (!full && !put_utf8(out, cap, &len, cp)) full = true;
    }
    out[len] = '\0';
    return true;
}

bool bl_jread_int(const char *text, const BlJTok *t, int64_t *out)
{
    if (t->type != BlJ_Prim) return false;
    int i = t->start;
    const bool neg = text[i] == '-';
    if (neg) i++;
    if (i >= t->end) return false;
    if (text[i] == '0' && i + 1 < t->end) return false;   // no leading zero
    uint64_t v = 0;
    for (; i < t->end; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t d = (uint64_t)(text[i] - '0');
        if (v > (UINT64_C(9223372036854775808) - d) / 10) return false;
        v = v * 10 + d;
    }
    if (!neg && v > (uint64_t)INT64_MAX) return false;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

bool bl_jread_bool(const char *text, const BlJTok *t, bool *out)
{
    if (t->type != BlJ_Prim) return false;
    const int n = t->end - t->start;
    if (n == 4 && !memcmp(text + t->start, "true", 4)) { *out = true; return true; }
    if (n == 5 && !memcmp(text + t->start, "false", 5)) { *out = false; return true; }
    return false;
}

size_t bl_jread_member(const char *text, const BlJTok *toks, int count, int key, char *out, size_t cap)
{
    if (key < 0 || key + 1 >= count || toks[key].type != BlJ_Str || toks[key].size != 1) return 0;
    // The key and the value with their quotes, without the blanks around ':'.
    const BlJTok *v = &toks[key + 1];
    const char *k = text + toks[key].start - 1;
    const size_t k_len = (size_t)(toks[key].end - toks[key].start) + 2;
    const bool quoted = v->type == BlJ_Str;
    const char *val = text + v->start - (quoted ? 1 : 0);
    const size_t v_len = (size_t)(v->end - v->start) + (quoted ? 2 : 0);
    if (k_len + 1 + v_len >= cap) return 0;
    memcpy(out, k, k_len);
    out[k_len] = ':';
    memcpy(out + k_len + 1, val, v_len);
    out[k_len + 1 + v_len] = '\0';
    return k_len + 1 + v_len;
}
