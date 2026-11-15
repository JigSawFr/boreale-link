// sync_exec — carries out an order (SyncIntent) on the console through the
// service layer (core/pctl_ops.h), exactly as a press on the console would:
// every write goes through core_change_allowed() (read-only mode) and the
// play-timer gate; when the timer counts down, parental controls are
// unlocked for the write with the stored PIN and locked again right after,
// also when the write failed. Shared by the agent sysmodule and PlayGuard
// (which then records the change in its history from the outcome).
//
// What has to survive between runs (extra time to put back, the console
// lock's saved limits, an unlock not yet locked again) is in SyncRecords:
// the agent keeps it in sync/agent_state.txt, PlayGuard in config.json.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "../core/pctl_ops.h"
#include "sync_apply.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Extra time / no more play today: weekday `extra_weekday` (-1: none)
    // went from `extra_base` to `extra_value` minutes on `extra_date`.
    int8_t   extra_weekday;
    char     extra_date[11];
    uint16_t extra_base, extra_value;
    // Console lock: on, and the seven limits it replaced (when known).
    bool     console_lock;
    bool     console_lock_prev_ok;
    uint16_t console_lock_prev[7];
    // Unlocked for a write that has not been locked again yet.
    bool     relock_pending;
} SyncRecords;

void sync_records_clear(SyncRecords *r);
// Flat "key=value" form (sync/agent_state.txt, sync/nro_state.txt).
size_t sync_records_write(const SyncRecords *r, char *out, size_t cap);
void   sync_records_parse(SyncRecords *r, const char *text, size_t len);

typedef struct {
    bool         remote_timer_writes;
    int          weekday;        // today, 0 = Sunday (the console's local time)
    const char  *today;          // "YYYY-MM-DD"
    SyncRecords *rec;            // read and updated
    // A saved profile's limits (Sunday first, PT_DAY_NOLIMIT for none).
    bool (*profile_days)(void *ctx, const char *name, uint16_t days[7]);
    // Saves the records on the SD card before a write that needs them there
    // first (an unlock to lock again, the extra time to put back, the console
    // lock and its saved limits), as PlayGuard's own changes do: false
    // refuses the order (NXM_RC_NOT_SAVED), nothing written; a write that
    // then fails has the previous records saved again. NULL: the host saves
    // `rec` after (it is updated either way).
    bool (*save_records)(void *ctx, const SyncRecords *rec);
    void *ctx;
} SyncExecCtx;

typedef enum {
    SyncChange_None = 0,
    SyncChange_Limits,       // before/after: 7 limits
    SyncChange_Level,        // 1 value
    SyncChange_Custom,       // 3 values: age, posting, communication
    SyncChange_Vr,           // 1 value
    SyncChange_Alarm,        // 1 value: 1 = alarm off (history's "alarm")
    SyncChange_Bedtime,      // before/after: alarm and end of day 0..6, as hh*60+mm, -1 off
    SyncChange_Unlock,
    SyncChange_Relock,
    SyncChange_ConsoleLock,  // 1 value: 0 off, 1 on
} SyncChange;

typedef struct {
    Result     rc;
    bool       applied;        // the console holds what was asked (already, or now)
    bool       changed;        // something was written
    SyncReason reason;         // why not, when !applied
    SyncChange change;
    int        n;              // values in before / after
    int        before[7], after[7];
    bool       did_unlock;     // unlocked for the write
    bool       relock_failed;  // and could not lock again: the console may still be unlocked
    bool       records_changed;
    int        console_lock_after;   // -1 unchanged, else the console lock's new state (0 / 1)
    // Where the change comes from, for the history ("remote", "remote_extra",
    // "remote_stop", "remote_console_lock", "remote_profile", "remote_restore_extra").
    const char *source;
} SyncOutcome;

void sync_outcome_init(SyncOutcome *o);

// Carries out a console intent (sync_intent_on_console). The caller has
// already checked the policy; this checks remote_timer_writes again.
void sync_exec(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out);

// Extra time or no more play added on an earlier day is still on its
// weekday: puts the usual limit back (as PlayGuard does the next day). Does
// nothing (returns false) when there is nothing to put back; drops the
// record when the limit changed since.
bool sync_exec_restore_extra(const SyncExecCtx *ctx, SyncOutcome *out);

#ifdef __cplusplus
}
#endif
