// sync_notices — the notices document (docs/sync-protocol.md, `notices`):
// the shared bell of every Boréale homebrew on the console, as their files
// under config/boreale/notices/ say (boreale-link's bl_notices), each under
// its name from the registry (config/boreale/apps/). Retained on
// <base>/notices, at the device's level: Home Assistant shows what waits,
// runs or failed in each homebrew, PlayGuard's own included. Built by
// PlayGuard and by the agent from the same files; a homebrew whose .nro is
// known and gone, an expired notice and a file of a newer schema are left
// out.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_NOTICES_SCHEMA 1

// The document, from the files under `sd_root` ("sdmc:/", "./sd/") at POSIX
// time `now`: its length, 0 when it does not fit in `cap`. Its buffers are
// static (a sysmodule's heap is small): one call at a time, the link's
// thread.
size_t sync_notices_build(const char *sd_root, int64_t now, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
