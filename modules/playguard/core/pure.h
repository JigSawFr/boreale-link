// pure — the parts of the service layer that never talk to a console: the
// PlayTimerSettings block codec, the names of levels and rating bodies, the
// firmware policy. Compiled for the Switch, for the desktop simulator
// (source/sim/) and for the host tests, so all three share one implementation.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once
#include "core/nx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// PlayTimerSettings (fw 21.0.0+): u16[34] (0x44 bytes), read with 145601 and
// written with 195101. Observed on a console with a real limit configured
// through the companion app (fw 22.1.0):
//   [0]   = 0x0101    observed header (non-zero <=> IsPlayTimerEnabled)
//   [1]   = 0x0001    ?
//   [2..6]= 0         reserved
//   7 per-day groups, group n at [7+4n .. 7+4n+3], Sun..Sat:
//     [+0] = 0x0600   ? (constant in the observed config)
//     [+1] = 0x0100   "this day has a configured limit" flag
//     [+2] = minutes  that day's limit
//     [+3] = 0        reserved (absent for the last group: the array stops at 34)
// A group left all-zero means "no limit that day".
//
// Read as bytes, the same block is four mode bytes, the companion app's
// "daily" rule and seven per-day rules, 8 bytes each (4 + 8 + 7 * 8 = 0x44).
// The layout of the 22.5.0 pctl sysmodule, as reversed by NX-Pctl-Manager
// (docs/parental-controls.md, "The settings block"):
//   byte 0  enabled (pctl clears it on a write and sets it again itself)
//   byte 1  restrictionMode: 0 ALARM (time's up: a warning only),
//           1 FORCED_TERMINATION (the software is suspended)
//   byte 2  timerMode: 0 DAILY (the daily rule, every day),
//           1 EACH_DAY_OF_THE_WEEK (the per-day rules)
//   byte 3  reserved
//   rule at byte 4 (daily) and 12 + 8n (day n, Sun..Sat), the companion
//   app's regulation (its "timeToPlayInOneDay" and "bedtime"):
//     +0 bedtime on   +1 alarm hour   +2 alarm minute
//     +3 allowed-again hour (the 0x06 of [+0])   +4 allowed-again minute
//     +5 limit flag (the 0x01 of [+1])           +6..7 limit minutes ([+2])
// so the "reserved" [6] and each [+3] hold the next day's bedtime switch and
// alarm hour. The bedtime fields come from that match (pt_bedtime_*), not
// from a console with a bedtime set: the write checks what the console then
// reports (1954/1956/1957) and puts the block back when it differs.
//
// Every block observed is EACH_DAY_OF_THE_WEEK (01 01 01 00), or all zero
// (the timer off, which reads as DAILY with nothing on). A DAILY block is
// read through its daily rule; a write that changes it moves it to
// EACH_DAY_OF_THE_WEEK first, the daily rule copied into the seven days
// (all-zero days when it has nothing on), so the console keeps doing the
// same but for the change. The restrictionMode is kept as read.
#define PT_U16_COUNT 34

// The mode bytes (byte offsets, above).
enum { PT_MODE_ENABLED = 0, PT_MODE_RESTRICTION = 1, PT_MODE_TIMER = 2 };

// restrictionMode is FORCED_TERMINATION: the software is suspended when the
// time is up (else a warning only).
bool pt_suspends(const u16 c[PT_U16_COUNT]);
// timerMode is DAILY: one rule (byte 4) for every day.
bool pt_daily(const u16 c[PT_U16_COUNT]);
// A limit or a bedtime is on, on some day: the play timer has something to
// do, and restrictionMode a meaning.
bool pt_configured(const u16 c[PT_U16_COUNT]);
// Sets restrictionMode (byte 1), nothing else; refused (false, the block
// unchanged) while nothing is configured: the "off" block is all zeros, and
// the next limit gives it the observed header (FORCED_TERMINATION).
bool pt_set_suspends(u16 c[PT_U16_COUNT], bool suspends);

// Per-day value: no configured limit that day (pctl_ops.h uses the same).
#ifndef PT_DAY_NOLIMIT
#define PT_DAY_NOLIMIT 0xFFFFu
#endif

// Sun..Sat minutes, or PT_DAY_NOLIMIT for a day without a flag: the daily
// rule's for every day in DAILY mode.
void pt_decode(const u16 c[PT_U16_COUNT], u16 days_min[7]);

// Whether the block can be the layout above at all: in each day and in the
// daily rule (bytes 04..0B), the bedtime and limit switches 0 or 1,
// hours below 24, minutes below 60, the limit 0..1440 minutes or
// PT_DAY_NOLIMIT. The four mode bytes are not checked: any value passes (a
// non-zero mode is read as 1). Only what that layout cannot hold is refused,
// so an unseen companion-app setting does not stop every write; a block that
// fails is one a firmware changed (or garbage), and nothing must be written
// from it.
bool pt_plausible(const u16 c[PT_U16_COUNT]);

// Turns the block read with 145601 into the one to write with 195101 for the
// per-day limits `days_min`, changing as little as possible:
//  - a day that keeps its limit gets the new minutes, nothing else changes;
//  - a day that gains a limit gets the observed 0x0600 / 0x0100 / minutes in
//    [+0..+2] (its [+3] stays);
//  - a day that loses its limit gets [+0..+2] cleared, the encoding seen
//    working on hardware (its [+3] stays);
//  - a day without a limit before and after is left exactly as read;
//  - the header stays as read, or gets the observed 0x0101 / 0x0001 when the
//    timer was off;
//  - a DAILY block whose limits change becomes EACH_DAY_OF_THE_WEEK first
//    (above), each day starting from the daily rule;
//  - no limit on any day: all zeros, the one "timer off" block seen working.
// So writing back the limits just read gives the same block, byte for byte.
void pt_encode(u16 c[PT_U16_COUNT], const u16 days_min[7]);
//  (A day whose bedtime is on keeps its times: only its limit flag and minutes
//  change, and "no limit on any day" keeps the block while a bedtime is on.)

// Sun..Sat limits a write may set: 0..1440 minutes, or PT_DAY_NOLIMIT.
bool pt_days_ok(const u16 days_min[7]);

// One day's bedtime (pure.h layout above): the alarm at hour:minute, play
// allowed again at end_hour:end_minute.
typedef struct {
    bool on;
    u8   hour, minute;
    u8   end_hour, end_minute;
} PtBedtime;

// The allowed range, as the companion app offers it: the alarm from 16:00 to
// 23:59, play allowed again from 05:00 to 09:00.
bool pt_bedtime_ok(const PtBedtime *b);

// Sun..Sat bedtimes as the block holds them (the daily rule's in DAILY mode).
void pt_bedtime_decode(const u16 c[PT_U16_COUNT], PtBedtime out[7]);

// Writes the seven bedtimes into the block read with 145601, changing only
// their bytes: a day turned off keeps its allowed-again time (as the
// companion app does) with the alarm cleared. A block that was off (header 0)
// gets the observed header when a bedtime is on; nothing on at all (no limit,
// no bedtime) gives all zeros, as pt_encode; a DAILY block that changes
// becomes EACH_DAY_OF_THE_WEEK first, as in pt_encode. Writing back the
// bedtimes just read gives the same block, byte for byte.
void pt_bedtime_encode(u16 c[PT_U16_COUNT], const PtBedtime in[7]);

// A PIN anyone would try first (4 to 8 digits): one digit repeated (0000),
// digits that go up or down by one (1234, 4321, 0123), a pair repeated
// (1212) or a column of the keypad (2580, 0852). `pin` holds `n` digits.
bool pin_is_weak(const char *pin, size_t n);

#ifdef __cplusplus
}
#endif
