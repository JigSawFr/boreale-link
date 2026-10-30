// bl_kv — the flat files' reader: "key=value", one per line, '#' starts a
// comment, a UTF-8 byte-order mark on the first line skipped. What the keys
// mean is the caller's. No allocation: a sysmodule reads its files with it.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Calls `fn` for each "key=value" line (key trimmed, value as written up to
// the line end, a trailing '\r' dropped). A key of 48 bytes or more, or a
// value of 512 or more, skips its line.
typedef void (*BlKvFn)(void *ctx, const char *key, const char *value);
void bl_kv_each(const char *text, size_t len, BlKvFn fn, void *ctx);

// Parses an unsigned integer with bounds; false when it is not one.
bool bl_parse_uint(const char *s, uint32_t min, uint32_t max, uint32_t *out);
// "1" / "true" / "on" / "yes" and "0" / "false" / "off" / "no", in any case.
bool bl_parse_bool(const char *s, bool *out);

#ifdef __cplusplus
}
#endif
