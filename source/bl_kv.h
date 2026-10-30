// bl_kv — the flat files' reader: "key=value", one per line, '#' starts a
// comment, a UTF-8 byte-order mark on the first line skipped. What the keys
// mean is the caller's. No allocation: a sysmodule reads its files with it.
//
// A file read and written back by several versions of a homebrew follows the
// `schema` rule: a `schema=N` line (none: 1, the files written before it), a
// file of a newer schema read for what is known of it but never written
// over, and the keys a version does not know written back as they were read,
// so that a version before or after it still finds them. BlKvFile does the
// bookkeeping:
//
//     char kept[1024];
//     BlKvFile f;
//     bl_kv_file_init(&f, kept, sizeof(kept));
//     bl_kv_read(&f, text, len, on_kv, &ctx);   // on_kv: bl_kv_keep(&f, …) for a key it does not know
//     …
//     if (bl_kv_may_rewrite(&f, MY_SCHEMA)) {
//         size_t n = snprintf(out, cap, "schema=%u\n…", MY_SCHEMA, …);   // its own keys
//         n = bl_kv_write_kept(&f, out, cap, n);                         // then the others, as read
//         if (n) save(out, n);
//     }
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the reader takes: a key under 48 bytes, a value under 512. A longer
// line is skipped, and counted.
#define BL_KV_KEY_MAX   48
#define BL_KV_VALUE_MAX 512

// The key of a file's format, and what a file without it is.
#define BL_KV_SCHEMA_KEY     "schema"
#define BL_KV_SCHEMA_ABSENT  1u

// Calls `fn` for each "key=value" line (key trimmed, value as written up to
// the line end, a trailing '\r' dropped). A key of BL_KV_KEY_MAX bytes or
// more, or a value of BL_KV_VALUE_MAX or more, skips its line: the result is
// the number of lines skipped so, 0 when every line was handed over.
typedef void (*BlKvFn)(void *ctx, const char *key, const char *value);
size_t bl_kv_each(const char *text, size_t len, BlKvFn fn, void *ctx);

// The file's `schema=` (spaces around the number allowed; the last line
// wins): BL_KV_SCHEMA_ABSENT without one, 0 when it is not a whole number
// from 1, which bl_kv_may_rewrite() takes as newer.
uint32_t bl_kv_schema(const char *text, size_t len);

// A file read for a round trip: its schema, the lines skipped, and the lines
// the caller did not know, kept in the caller's buffer as "key=value\n" (a C
// string) to be written back.
typedef struct {
    uint32_t schema;    // as bl_kv_schema()
    size_t   skipped;   // lines too long to be read (bl_kv_each)
    char    *kept;      // the caller's buffer
    size_t   cap;
    size_t   len;       // bytes of `kept` used, its '\0' not counted
    bool     overflow;  // a line to keep did not fit: some are lost
} BlKvFile;

void bl_kv_file_init(BlKvFile *f, char *kept, size_t cap);

// bl_kv_each() over the file, minus its `schema=` lines (in f->schema), with
// f->skipped set. `fn` calls bl_kv_keep() for each key it does not know.
// Resets what an earlier read kept.
void bl_kv_read(BlKvFile *f, const char *text, size_t len, BlKvFn fn, void *ctx);

// Keeps a line to write back as read; f->overflow when it does not fit.
void bl_kv_keep(BlKvFile *f, const char *key, const char *value);

// Whether the writer of schema `mine` may write the file over: not when its
// schema is newer (or unreadable), nor when a line was skipped or a line to
// keep did not fit, since writing would lose it. The file is then left as it
// is: the caller works with what it read, in memory.
bool bl_kv_may_rewrite(const BlKvFile *f, uint32_t mine);

// Appends the kept lines to `out` from `at` (the caller's own keys before):
// the new length, or 0 when they do not fit in `cap` with a '\0'.
size_t bl_kv_write_kept(const BlKvFile *f, char *out, size_t cap, size_t at);

// Parses an unsigned integer with bounds; false when it is not one.
bool bl_parse_uint(const char *s, uint32_t min, uint32_t max, uint32_t *out);
// "1" / "true" / "on" / "yes" and "0" / "false" / "off" / "no", in any case.
bool bl_parse_bool(const char *s, bool *out);

#ifdef __cplusplus
}
#endif
