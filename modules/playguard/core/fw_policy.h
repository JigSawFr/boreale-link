// fw_policy — PlayGuard's firmware policy: the range of firmware its play-timer
// layout is known on, and what the console's firmware makes of it
// (sysinfo_compat, in pure.c). The firmware itself is the kit's core/sysinfo.h.
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once
#include "core/sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

// Firmware policy. PlayTimerSettings (cmds 145601 / 195101) has been 0x44 bytes
// since 21.0.0 — the only layout this tool knows how to read and write.
#define PCTL_FW_MIN_PLAYTIMER MAKEHOSVERSION(21, 0, 0)
// Newest firmware PlayGuard was verified on, on a console (the observations of
// docs/parental-controls.md). Above it the firmware gate asks first (fw_gate),
// and compat.json publishes it (tools/gen_compat.py): raise it only with a
// trace from that firmware.
#define PCTL_FW_TESTED_MAX    MAKEHOSVERSION(23, 0, 1)
// Newest firmware the command table was checked against (switchbrew, Oct 2026):
// what the IDs in pctl_ops.c are known to match, not a test. Information only.
#define PCTL_FW_TABLE_CHECKED MAKEHOSVERSION(23, 0, 1)

typedef enum {
    SysCompat_Ok = 0,              // within the range verified on a console
    SysCompat_UntestedNewer,       // newer than PCTL_FW_TESTED_MAX: reads fine, writes at own risk
    SysCompat_PlayTimerUnsupported,// older than 21.0.0: play-timer features disabled
    SysCompat_NotAtmosphere,       // not running under Atmosphère
} SysCompat;

SysCompat sysinfo_compat(const SysInfo *info);

#ifdef __cplusplus
}
#endif
