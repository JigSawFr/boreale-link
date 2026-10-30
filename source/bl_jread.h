// bl_jread — a JSON reader for the documents a homebrew reads from another
// one (the shared files of config/boreale/). No allocation: the text is cut
// into tokens kept in the caller's array, in document order, each with where
// it starts and ends in the text; the caller walks them. A sysmodule reads
// with it as well.
//
//     BlJTok t[256];
//     const int n = bl_jread_parse(text, len, t, 256);
//     if (n < 1 || t[0].type != BlJ_Obj) …;                  // not a document
//     for (int i = 1, k = 0; k < t[0].size; k++) {           // the members
//         const int key = i, val = i + 1;
//         if (bl_jread_eq(text, &t[key], "schema")) bl_jread_int(text, &t[val], &schema);
//         i = bl_jread_skip(t, n, val);                      // the next key
//     }
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BlJ_Obj,
    BlJ_Arr,
    BlJ_Str,    // start and end without the quotes, escapes as written
    BlJ_Prim,   // a number, true, false or null
} BlJType;

typedef struct {
    BlJType type;
    int     start, end;   // [start, end) in the text
    int     size;         // an object: its keys; an array: its values; a key: 1 (its value)
    int     parent;       // the token it is in, -1 for the document
} BlJTok;

// The tokens of `text` into `toks`: their count, or -1 when it is not JSON
// (or not whole), -2 when `n` tokens are not enough.
int bl_jread_parse(const char *text, size_t len, BlJTok *toks, int n);

// The token after `i` and everything in it (its value, for a key).
int bl_jread_skip(const BlJTok *toks, int count, int i);

// A string token equal to `s` (compared as written: keys have no escapes).
bool bl_jread_eq(const char *text, const BlJTok *t, const char *s);
// A string token's value, unescaped into `out` (UTF-8, '\0' ended), cut at
// a character's boundary when it does not fit: false for another token or a
// bad escape.
bool bl_jread_str(const char *text, const BlJTok *t, char *out, size_t cap);
// A whole number (no fraction, no exponent) within int64_t.
bool bl_jread_int(const char *text, const BlJTok *t, int64_t *out);
// true or false.
bool bl_jread_bool(const char *text, const BlJTok *t, bool *out);

// The member whose key is token `key`, as written ("\"key\":value"), to
// keep it when the document is written again: the length copied, 0 when
// it does not fit in `cap` with a '\0'.
size_t bl_jread_member(const char *text, const BlJTok *toks, int count, int key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
