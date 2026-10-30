// bl_json — a JSON writer into a fixed buffer, for the documents a homebrew
// publishes. No allocation: a sysmodule links it with a static heap. Once
// the buffer is full every call is a no-op and bl_json_end() returns 0, so a
// caller checks once at the end. Keys and values are escaped; commas are
// placed automatically.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_JSON_DEPTH 12

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   overflow;
    int    depth;
    bool   first[BL_JSON_DEPTH];   // no member written yet at that depth
    bool   after_key;                // a key was just written: no comma before the value
} BlJson;

void bl_json_init(BlJson *j, char *buf, size_t cap);
// Length written (NUL-terminated), or 0 when it did not fit or is unbalanced.
size_t bl_json_end(BlJson *j);

void bl_json_obj(BlJson *j);       // {
void bl_json_obj_end(BlJson *j);   // }
void bl_json_arr(BlJson *j);       // [
void bl_json_arr_end(BlJson *j);   // ]
void bl_json_key(BlJson *j, const char *key);

void bl_json_str(BlJson *j, const char *s);
void bl_json_strn(BlJson *j, const char *s, size_t n);
void bl_json_int(BlJson *j, int64_t v);
void bl_json_uint(BlJson *j, uint64_t v);
void bl_json_bool(BlJson *j, bool v);
void bl_json_null(BlJson *j);
// Pre-formatted JSON (a number, an object built elsewhere), written as is.
void bl_json_raw(BlJson *j, const char *json);
// Members already written as JSON ("\"a\":1,\"b\":true"), added to the
// object being written.
void bl_json_members(BlJson *j, const char *members);
// 16 hex digits, upper case, as a string ("0100000000010000").
void bl_json_hex64(BlJson *j, uint64_t v);

// key + value in one call.
void bl_json_kstr(BlJson *j, const char *key, const char *s);
void bl_json_kint(BlJson *j, const char *key, int64_t v);
void bl_json_kbool(BlJson *j, const char *key, bool v);
void bl_json_knull(BlJson *j, const char *key);
// The value when `ok`, else null.
void bl_json_kint_or_null(BlJson *j, const char *key, bool ok, int64_t v);
void bl_json_kbool_or_null(BlJson *j, const char *key, bool ok, bool v);
void bl_json_kstr_or_null(BlJson *j, const char *key, const char *s);   // NULL or "" -> null

#ifdef __cplusplus
}
#endif
