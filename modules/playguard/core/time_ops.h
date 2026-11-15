// time_ops — read the system clocks and write the network clock (time:s).
// All handles are acquired and released within each call.
// Adapted from anbingxi/NX-Pctl-Manager (diag/fw22-5-readonly).
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once
#include "core/calendar.h"
#include "core/local_time.h"
#include "core/nx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Result service_rc, user_rc, network_rc, local_rc, automatic_rc, accuracy_rc, location_rc;
    u64  user_time, network_time, local_time;     // POSIX seconds (UTC)
    bool automatic;   // 100 IsStandardUserSystemClockAutomaticCorrectionEnabled
    bool accuracy;    // 200 IsStandardNetworkSystemClockAccuracySufficient
    char location[0x25];                          // time zone, e.g. "Europe/Paris"
} TimeSnapshot;

typedef struct {
    TimeSnapshot before, after;
    bool   refused_automatic, write_attempted, verify_attempted, verified;
    Result open_rc, write_rc, verify_rc;
    u64    target;     // what was written (or would have been)
    u64    readback;
} TimeApply;

void time_clock_snapshot(TimeSnapshot *out);
// Only 200 IsStandardNetworkSystemClockAccuracySufficient: one command instead
// of the snapshot's three clock sessions, for Today's periodic refresh.
Result time_network_accuracy(bool *accurate);
// Writes utc_seconds to the standard network clock. Refuses (refused_automatic)
// when automatic correction is disabled or unknown: the user clock would then
// not follow, and the user asked for the system's own setting to stay in charge.
void time_clock_apply(u64 utc_seconds, TimeApply *out);
// Sets the network clock to the user clock, read in the same session just
// before the write: for a console no time server answers. No refusal when
// automatic correction is off: the user clock is the source and stays where
// it is, and turning correction on afterwards no longer moves it. The user
// clock read fails: open_rc, nothing written.
void time_clock_set_to_user(TimeApply *out);
void time_clock_dump(char *buf, size_t size);

// The console's time-zone rule, the user clock now and its local time, and
// a time formatted in the console's time zone are the kit's (core/local_time.h).

// The standard steady clock: seconds since the console's RTC was set up,
// kept through sleep mode and restarts; changing the console clock does not
// move it. `source_id` (16 bytes, may be NULL) names its source: values from
// two sources do not compare. Read only.
Result time_steady_now(u64 *seconds, u8 source_id[16]);

#ifdef __cplusplus
}
#endif
