// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "time_ops.h"
#include "core/calendar.h"
#include "core/write_guard.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// IStaticService (time:s): 0 GetStandardUserSystemClock, 1 GetStandardNetworkSystemClock,
// 3 GetTimeZoneService, 4 GetStandardLocalSystemClock,
// 100 IsStandardUserSystemClockAutomaticCorrectionEnabled,
// 200 IsStandardNetworkSystemClockAccuracySufficient.
// ISystemClock: 0 GetCurrentTime, 1 SetCurrentTime. ITimeZoneService: 0 GetDeviceLocationName.

static Result open_sub(Service *root, Service *sub, u32 command)
{
    return serviceDispatch(root, command, .out_num_objects = 1, .out_objects = sub);
}

static Result read_clock(Service *root, u32 command, u64 *value)
{
    Service clock = {0};
    Result rc = open_sub(root, &clock, command);
    if (R_SUCCEEDED(rc)) rc = serviceDispatchOut(&clock, 0, *value);
    serviceClose(&clock);
    return rc;
}

static Result read_location(Service *root, char *out, size_t size)
{
    Service tz = {0};
    char name[0x24];
    memset(name, 0, sizeof(name));
    Result rc = open_sub(root, &tz, 3);
    if (R_SUCCEEDED(rc)) rc = serviceDispatchOut(&tz, 0, name);
    serviceClose(&tz);
    if (R_SUCCEEDED(rc)) {
        size_t n = sizeof(name) < size - 1 ? sizeof(name) : size - 1;
        memcpy(out, name, n);
        out[n] = '\0';
    }
    return rc;
}

void time_clock_snapshot(TimeSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    Service root = {0};
    out->service_rc = smGetService(&root, "time:s");
    out->user_rc = out->network_rc = out->local_rc = out->automatic_rc =
        out->accuracy_rc = out->location_rc = out->service_rc;
    if (R_SUCCEEDED(out->service_rc)) {
        out->user_rc     = read_clock(&root, 0, &out->user_time);
        out->network_rc  = read_clock(&root, 1, &out->network_time);
        out->local_rc    = read_clock(&root, 4, &out->local_time);
        u8 b = 0;
        out->automatic_rc = serviceDispatchOut(&root, 100, b);
        out->automatic = b != 0;
        b = 0;
        out->accuracy_rc = serviceDispatchOut(&root, 200, b);
        out->accuracy = b != 0;
        out->location_rc = read_location(&root, out->location, sizeof(out->location));
    }
    serviceClose(&root);
}

Result time_network_accuracy(bool *accurate)
{
    *accurate = false;
    Service root = {0};
    // time:s, else time:u (the agent's): the command is the public API's.
    Result rc = smGetService(&root, "time:s");
    if (R_FAILED(rc)) rc = smGetService(&root, "time:u");
    if (R_SUCCEEDED(rc)) {
        u8 b = 0;
        rc = serviceDispatchOut(&root, 200, b);
        if (R_SUCCEEDED(rc)) *accurate = b != 0;
    }
    serviceClose(&root);
    return rc;
}

// Writes and reads back the network clock (`network` is open); fills the
// write and verify fields of `out`.
static void write_network(Service *network, u64 utc_seconds, TimeApply *out)
{
    out->target = utc_seconds;
    out->write_attempted = true;
    out->write_rc = serviceDispatchIn(network, 1, utc_seconds);
    if (R_SUCCEEDED(out->write_rc)) {
        out->verify_attempted = true;
        out->verify_rc = serviceDispatchOut(network, 0, out->readback);
        // Allow the small interval elapsed during IPC; never underflow.
        out->verified = R_SUCCEEDED(out->verify_rc) && out->readback >= utc_seconds &&
                        out->readback - utc_seconds <= 5;
    }
}

void time_clock_apply(u64 utc_seconds, TimeApply *out)
{
    memset(out, 0, sizeof(*out));
    time_clock_snapshot(&out->before);
    out->target = utc_seconds;
    const Result gate = core_change_allowed();   // read-only, or the PIN asked first
    if (R_FAILED(gate)) {
        out->open_rc = gate;
        time_clock_snapshot(&out->after);
        return;
    }
    if (R_FAILED(out->before.automatic_rc) || !out->before.automatic) {
        out->refused_automatic = true;
        out->after = out->before;
        return;
    }
    Service root = {0}, network = {0};
    out->open_rc = smGetService(&root, "time:s");
    if (R_SUCCEEDED(out->open_rc)) out->open_rc = open_sub(&root, &network, 1);
    if (R_SUCCEEDED(out->open_rc)) write_network(&network, utc_seconds, out);
    serviceClose(&network);
    serviceClose(&root);
    time_clock_snapshot(&out->after);
}

void time_clock_set_to_user(TimeApply *out)
{
    memset(out, 0, sizeof(*out));
    time_clock_snapshot(&out->before);
    out->target = out->before.user_time;
    const Result gate = core_change_allowed();   // read-only, or the PIN asked first
    if (R_FAILED(gate)) {
        out->open_rc = gate;
        time_clock_snapshot(&out->after);
        return;
    }
    Service root = {0}, network = {0};
    u64 user = 0;
    out->open_rc = smGetService(&root, "time:s");
    // Read now, not at the confirmation: the user clock went on meanwhile.
    if (R_SUCCEEDED(out->open_rc)) out->open_rc = read_clock(&root, 0, &user);
    if (R_SUCCEEDED(out->open_rc)) out->open_rc = open_sub(&root, &network, 1);
    if (R_SUCCEEDED(out->open_rc)) write_network(&network, user, out);
    serviceClose(&network);
    serviceClose(&root);
    time_clock_snapshot(&out->after);
}

// ---------------------------------------------------------------- local time

Result time_steady_now(u64 *seconds, u8 source_id[16])
{
    TimeSteadyClockTimePoint tp;
    memset(&tp, 0, sizeof(tp));
    const Result rc = timeGetStandardSteadyClockTimePoint(&tp);
    if (R_FAILED(rc)) return rc;
    if (seconds) *seconds = tp.time_point > 0 ? (u64)tp.time_point : 0;
    if (source_id) memcpy(source_id, &tp.source_id, 16);
    return 0;
}

// ---------------------------------------------------------------- dump

static const char *flag(Result rc, bool v) { return R_FAILED(rc) ? "unavailable" : (v ? "true" : "false"); }

void time_clock_dump(char *buf, size_t size)
{
    TimeSnapshot s;
    time_clock_snapshot(&s);
    snprintf(buf, size,
        "=== System clocks (UTC POSIX seconds) ===\n"
        "time:s open:                         rc=0x%08X\n"
        "User clock:                          rc=0x%08X value=%llu\n"
        "Network clock:                       rc=0x%08X value=%llu\n"
        "Local clock:                         rc=0x%08X value=%llu\n"
        "100 Automatic correction enabled:    rc=0x%08X %s\n"
        "200 Network clock accuracy sufficient: rc=0x%08X %s\n"
        "Time zone:                           rc=0x%08X %s\n"
        "Clock service handles released.\n\n",
        (unsigned)s.service_rc,
        (unsigned)s.user_rc, (unsigned long long)s.user_time,
        (unsigned)s.network_rc, (unsigned long long)s.network_time,
        (unsigned)s.local_rc, (unsigned long long)s.local_time,
        (unsigned)s.automatic_rc, flag(s.automatic_rc, s.automatic),
        (unsigned)s.accuracy_rc, flag(s.accuracy_rc, s.accuracy),
        (unsigned)s.location_rc, R_SUCCEEDED(s.location_rc) ? s.location : "-");
}
