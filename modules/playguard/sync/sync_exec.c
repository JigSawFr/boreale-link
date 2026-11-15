// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_exec.h"

#include "sync_state.h"

#include <stdio.h>
#include <string.h>

#include "core/write_guard.h"
#include "sync_conf.h"

#define NOLIMIT PT_DAY_NOLIMIT

// ---- the decisions of action/pt_logic.cpp, in C ----

static bool state_known(const PtState *pt)
{
    return pt->valid && pt->enabled_valid && pt->restricted_valid && pt->temporary_unlocked_valid;
}

static bool needs_unlock(const PtState *pt)
{
    return (pt->enabled || pt->restricted) && !pt->temporary_unlocked;
}

static bool reported(const PtState *pt, const PtBedtime *b)
{
    return b->on == pt->bedtime_enabled && (!b->on || (b->hour == pt->bedtime_hour && b->minute == pt->bedtime_minute));
}

static bool bedtime_layout_ok(const PtState *pt, int weekday)
{
    if (weekday < 0 || weekday > 6 || !pt->fw_supported || !pt->valid || !pt->bedtime_valid) return false;
    return reported(pt, &pt->bed[weekday]) || reported(pt, &pt->bed[(weekday + 6) % 7]);
}

static bool end_in_range(uint8_t hour, uint8_t minute)
{
    const int t = hour * 60 + minute;
    return minute < 60 && t >= 5 * 60 && t <= 9 * 60;
}

// ---- outcome helpers ----

static void refuse(SyncOutcome *out, SyncReason reason, Result rc)
{
    out->applied = false;
    out->reason = reason;
    out->rc = rc;
}

// The reason a service-layer result gives.
static SyncReason reason_of(Result rc)
{
    if (rc == NXM_RC_READ_ONLY) return SyncReason_ReadOnly;
    if (rc == NXM_RC_NOT_CONFIRMED) return SyncReason_NotConfirmed;
    if (rc == NXM_RC_WRITE_GATED) return SyncReason_Gated;
    if (rc == NXM_RC_NOT_CUSTOM) return SyncReason_NotCustom;
    if (rc == NXM_RC_FW_UNSUPPORTED || rc == NXM_RC_NOT_APPLIED) return SyncReason_Unsupported;
    if (rc == NXM_RC_UNLOCK_NOT_EFFECTIVE || rc == NXM_RC_RELOCK_FAILED || rc == NXM_RC_NO_PIN) return SyncReason_UnlockFailed;
    if (rc == NXM_RC_INVALID_ARGUMENT) return SyncReason_OutOfRange;
    return SyncReason_PctlError;
}

static void done(SyncOutcome *out, Result rc, bool wrote)
{
    out->rc = rc;
    if (R_SUCCEEDED(rc)) {
        out->applied = true;
        out->changed = wrote;
        out->reason = SyncReason_None;
    } else {
        out->applied = false;
        out->changed = false;
        out->reason = reason_of(rc);
    }
}

// ---- gated play-timer writes ----

typedef enum { W_DAYS, W_BEDTIME, W_ALARM } WriteKind;

typedef struct {
    WriteKind kind;
    uint16_t  days[7];
    PtBedtime bed[7];
    int       weekday;
    bool      alarm_disabled;
} Write;

static Result do_write(const Write *w)
{
    switch (w->kind) {
        case W_DAYS: return pctl_play_timer_set_days(w->days);
        case W_BEDTIME: return pctl_play_timer_set_bedtime(w->bed, w->weekday);
        case W_ALARM: return pctl_play_timer_set_alarm_disabled(w->alarm_disabled);
    }
    return NXM_RC_INVALID_ARGUMENT;
}

static bool save_first(const SyncExecCtx *ctx, const SyncRecords *r)
{
    return !ctx->save_records || ctx->save_records(ctx->ctx, r);
}

// The records as they were before the write (`relock_pending` as given),
// saved again.
static void put_back(const SyncExecCtx *ctx, const SyncRecords *prev, bool relock_pending)
{
    *ctx->rec = *prev;
    ctx->rec->relock_pending = relock_pending;
    save_first(ctx, ctx->rec);
}

// As pt_flow::confirm_write + finish_write with "lock again automatically":
// unlock when the timer counts down, write, lock again even when the write
// failed. The unlock is recorded until the lock succeeded, so the next start
// of either process locks again after a crash. `next`, when not NULL, is
// what the records become with this write: saved with the unlock's record
// before anything changes (ctx->save_records), and put back when the write
// does not go through.
static Result gated(const SyncExecCtx *ctx, SyncOutcome *out, const Write *w, const SyncRecords *next)
{
    PtState pt;
    pctl_play_timer_query(&pt);
    if (!pt.fw_supported) return NXM_RC_FW_UNSUPPORTED;
    if (!state_known(&pt)) return NXM_RC_STATE_UNKNOWN;
    // Read-only refuses before anything is unlocked.
    const Result allowed = core_change_allowed();
    if (R_FAILED(allowed)) return allowed;
    const bool unlock = needs_unlock(&pt);
    const SyncRecords prev = *ctx->rec;
    if (unlock || next) {
        SyncRecords want = next ? *next : prev;
        if (unlock) want.relock_pending = true;
        if (!save_first(ctx, &want)) return NXM_RC_NOT_SAVED;
        *ctx->rec = want;
        out->records_changed = true;
    }
    if (unlock) {
        const Result rc = pctl_unlock_restriction_temporarily();
        if (R_FAILED(rc)) {
            // Not unlocked, or locked again by the service layer: the record
            // goes only when the console reads back as locked.
            bool unlocked = true;
            const bool locked = R_SUCCEEDED(pctl_lock_state(NULL, &unlocked)) && !unlocked;
            put_back(ctx, &prev, locked ? prev.relock_pending : true);
            return rc == NXM_RC_WRITE_GATED ? NXM_RC_UNLOCK_NOT_EFFECTIVE : rc;
        }
        out->did_unlock = true;
    }
    const Result rc = do_write(w);
    if (unlock) {
        if (R_SUCCEEDED(pctl_relock())) ctx->rec->relock_pending = false;
        else out->relock_failed = true;
    }
    if (R_FAILED(rc) && next) put_back(ctx, &prev, ctx->rec->relock_pending);
    return rc;
}

static void days_values(const uint16_t days[7], int out[7])
{
    for (int d = 0; d < 7; d++) out[d] = days[d];
}

// Writes `days` (all seven) when they differ from what the console has;
// `next`: what the records become with it (gated()).
static void write_limits(const SyncExecCtx *ctx, SyncOutcome *out, const PtState *pt, const uint16_t days[7],
                         const SyncRecords *next)
{
    out->change = SyncChange_Limits;
    out->n = 7;
    days_values(pt->day_min, out->before);
    days_values(days, out->after);
    if (!memcmp(pt->day_min, days, sizeof(pt->day_min))) {
        done(out, 0, false);
        return;
    }
    Write w;
    memset(&w, 0, sizeof(w));
    w.kind = W_DAYS;
    memcpy(w.days, days, sizeof(w.days));
    const Result rc = gated(ctx, out, &w, next);
    done(out, rc, true);
}

// The current limits, or a refusal.
static bool read_timer(SyncOutcome *out, PtState *pt)
{
    pctl_play_timer_query(pt);
    if (!pt->fw_supported) {
        refuse(out, SyncReason_Unsupported, NXM_RC_FW_UNSUPPORTED);
        return false;
    }
    if (!pt->valid) {
        refuse(out, SyncReason_PctlError, pt->config_rc ? pt->config_rc : NXM_RC_STATE_UNKNOWN);
        return false;
    }
    return true;
}

static void exec_limits(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out, uint8_t mask)
{
    PtState pt;
    if (!read_timer(out, &pt)) return;
    uint16_t days[7];
    memcpy(days, pt.day_min, sizeof(days));
    for (int d = 0; d < 7; d++)
        if (mask & (1u << d)) days[d] = in->days[d];
    // Limits written by anything but the console lock replace it.
    SyncRecords next = *ctx->rec;
    const bool had_lock = next.console_lock || next.console_lock_prev_ok;
    next.console_lock = false;
    next.console_lock_prev_ok = false;
    write_limits(ctx, out, &pt, days, had_lock ? &next : NULL);
    if (had_lock && out->applied && out->changed) out->console_lock_after = 0;
}

static void exec_console_lock(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out)
{
    SyncRecords *r = ctx->rec;
    out->source = "remote_console_lock";
    if (in->on == r->console_lock) {
        done(out, 0, false);
        return;
    }
    PtState pt;
    if (!read_timer(out, &pt)) return;
    uint16_t days[7];
    // The limits to put back are on the SD card before the console changes.
    SyncRecords next = *r;
    if (in->on) {
        memset(days, 0, sizeof(days));
        next.console_lock = true;
        next.console_lock_prev_ok = true;
        memcpy(next.console_lock_prev, pt.day_min, sizeof(pt.day_min));
    } else {
        bool any = false;
        for (int d = 0; d < 7; d++) any = any || (r->console_lock_prev_ok && r->console_lock_prev[d] != NOLIMIT);
        for (int d = 0; d < 7; d++) days[d] = any ? r->console_lock_prev[d] : (uint16_t)NOLIMIT;
        next.console_lock = false;
        next.console_lock_prev_ok = false;
    }
    write_limits(ctx, out, &pt, days, &next);
    if (!out->applied) return;
    next.relock_pending = r->relock_pending;   // as the write left it
    *r = next;   // also when the console already had those limits (nothing written)
    out->records_changed = true;
    out->console_lock_after = in->on ? 1 : 0;
}

static bool extra_again(const SyncExecCtx *ctx, int wd)
{
    return ctx->rec->extra_weekday == wd && ctx->today && !strcmp(ctx->rec->extra_date, ctx->today);
}

// What the records become with extra time on weekday `wd`: what to put back.
static SyncRecords with_extra(const SyncExecCtx *ctx, int wd, uint16_t base, uint16_t value)
{
    SyncRecords r = *ctx->rec;
    r.extra_weekday = (int8_t)wd;
    snprintf(r.extra_date, sizeof(r.extra_date), "%s", ctx->today ? ctx->today : "");
    r.extra_base = base;
    r.extra_value = value;
    return r;
}

static void exec_bonus(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out)
{
    out->source = "remote_extra";
    if (ctx->rec->console_lock) {
        refuse(out, SyncReason_ConsoleLocked, 0);
        return;
    }
    const int wd = ctx->weekday;
    PtState pt;
    if (!read_timer(out, &pt)) return;
    if (wd < 0 || wd > 6 || !pt.enabled_valid || !pt.enabled || pt.day_min[wd] == NOLIMIT || pt.day_min[wd] >= 1440) {
        refuse(out, SyncReason_NoLimitToday, 0);
        return;
    }
    const uint16_t base = pt.day_min[wd];
    const int sum = base + in->minutes;
    const uint16_t value = (uint16_t)(sum > 1440 ? 1440 : sum);
    const uint16_t original = extra_again(ctx, wd) ? ctx->rec->extra_base : base;
    uint16_t days[7];
    memcpy(days, pt.day_min, sizeof(days));
    days[wd] = value;
    const SyncRecords next = with_extra(ctx, wd, original, value);
    write_limits(ctx, out, &pt, days, &next);
}

static void exec_stop_today(const SyncExecCtx *ctx, SyncOutcome *out)
{
    out->source = "remote_stop";
    if (ctx->rec->console_lock) {
        refuse(out, SyncReason_ConsoleLocked, 0);
        return;
    }
    const int wd = ctx->weekday;
    PtState pt;
    if (!read_timer(out, &pt)) return;
    if (wd < 0 || wd > 6) {
        refuse(out, SyncReason_NoLimitToday, 0);
        return;
    }
    const uint16_t base = pt.day_min[wd];
    const uint16_t original = extra_again(ctx, wd) ? ctx->rec->extra_base : base;
    uint16_t days[7];
    memcpy(days, pt.day_min, sizeof(days));
    days[wd] = 0;
    const SyncRecords next = with_extra(ctx, wd, original, 0);
    write_limits(ctx, out, &pt, days, &next);
}

static void exec_bedtime(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out)
{
    out->change = SyncChange_Bedtime;
    PtState pt;
    if (!read_timer(out, &pt)) return;
    if (!bedtime_layout_ok(&pt, ctx->weekday)) {
        refuse(out, SyncReason_Unsupported, 0);
        return;
    }
    PtBedtime bed[7];
    bool any_on = false;
    for (int d = 0; d < 7; d++) any_on = any_on || pt.bed[d].on;
    for (int d = 0; d < 7; d++) {
        PtBedtime b = pt.bed[d];
        if (in->kind == SyncIntent_BedtimeEnd) {
            if (b.on) {
                b.end_hour = in->hour;
                b.end_minute = in->minute;
            }
        } else {
            // On at a time (the alarm entity, or the switch: the alarm the
            // days already share, else 21:00), or off.
            bool on = in->kind == SyncIntent_Bedtime ? true : in->on;
            uint8_t hour = in->hour, minute = in->minute;
            if (in->kind == SyncIntent_BedtimeEnabled) {
                hour = 21;
                minute = 0;
                for (int k = 0; k < 7; k++)
                    if (pt.bed[k].on) {
                        hour = pt.bed[k].hour;
                        minute = pt.bed[k].minute;
                        break;
                    }
                if (on && b.on) {
                    bed[d] = b;
                    continue;
                }
            }
            b.on = on;
            b.hour = on ? hour : 0;
            b.minute = on ? minute : 0;
            if (on && !end_in_range(b.end_hour, b.end_minute)) {
                b.end_hour = 6;
                b.end_minute = 0;
            }
        }
        bed[d] = b;
    }
    if (in->kind == SyncIntent_BedtimeEnd && !any_on) {
        refuse(out, SyncReason_BedtimeOff, 0);
        return;
    }
    for (int d = 0; d < 7; d++)
        if (bed[d].on && !pt_bedtime_ok(&bed[d])) {
            refuse(out, SyncReason_OutOfRange, NXM_RC_INVALID_ARGUMENT);
            return;
        }
    bool same = true;
    for (int d = 0; d < 7 && same; d++) {
        const PtBedtime *a = &pt.bed[d], *b = &bed[d];
        same = a->on == b->on && (!a->on || (a->hour == b->hour && a->minute == b->minute &&
                                            a->end_hour == b->end_hour && a->end_minute == b->end_minute));
    }
    if (same) {
        done(out, 0, false);
        return;
    }
    Write w;
    memset(&w, 0, sizeof(w));
    w.kind = W_BEDTIME;
    memcpy(w.bed, bed, sizeof(w.bed));
    w.weekday = ctx->weekday;
    done(out, gated(ctx, out, &w, NULL), true);
}

static void exec_alarm(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out)
{
    out->change = SyncChange_Alarm;
    out->n = 1;
    PtState pt;
    pctl_play_timer_query(&pt);
    if (!pt.fw_supported) {
        refuse(out, SyncReason_Unsupported, NXM_RC_FW_UNSUPPORTED);
        return;
    }
    const bool disabled = !in->on;
    out->after[0] = disabled;
    out->before[0] = pt.alarm_disabled_valid ? pt.alarm_disabled : -1;
    if (pt.alarm_disabled_valid && pt.alarm_disabled == disabled) {
        done(out, 0, false);
        return;
    }
    Write w;
    memset(&w, 0, sizeof(w));
    w.kind = W_ALARM;
    w.alarm_disabled = disabled;
    done(out, gated(ctx, out, &w, NULL), true);
}

static void exec_restrictions(const SyncIntent *in, SyncOutcome *out)
{
    PctlStatus st;
    pctl_status_fetch(&st);
    if (in->kind == SyncIntent_Level) {
        out->change = SyncChange_Level;
        out->n = 1;
        out->before[0] = st.safety_level_ok ? (int)st.safety_level : -1;
        out->after[0] = (int)in->level;
        if (st.safety_level_ok && st.safety_level == in->level) {
            done(out, 0, false);
            return;
        }
        done(out, pctl_set_safety_level(in->level), true);
        return;
    }
    if (in->kind == SyncIntent_Vr) {
        out->change = SyncChange_Vr;
        out->n = 1;
        out->before[0] = st.stereo_vision_ok ? st.stereo_vision_restricted : -1;
        out->after[0] = in->on;
        if (st.stereo_vision_ok && st.stereo_vision_restricted == in->on) {
            done(out, 0, false);
            return;
        }
        done(out, pctl_set_stereo_vision_restricted(in->on), true);
        return;
    }
    // Posting or communication: the custom level's settings.
    out->change = SyncChange_Custom;
    out->n = 3;
    if (!st.settings_ok || !st.safety_level_ok) {
        refuse(out, SyncReason_PctlError, st.settings_ok ? NXM_RC_STATE_UNKNOWN : st.session_rc);
        return;
    }
    if (st.safety_level != PctlSafetyLevel_Custom) {
        refuse(out, SyncReason_NotCustom, NXM_RC_NOT_CUSTOM);
        return;
    }
    PctlCustomSettings s = st.settings;
    out->before[0] = s.rating_age;
    out->before[1] = s.sns_post_restriction;
    out->before[2] = s.free_communication_restriction;
    if (in->kind == SyncIntent_Sns) s.sns_post_restriction = in->on;
    else s.free_communication_restriction = in->on;
    out->after[0] = s.rating_age;
    out->after[1] = s.sns_post_restriction;
    out->after[2] = s.free_communication_restriction;
    if (s.sns_post_restriction == st.settings.sns_post_restriction &&
        s.free_communication_restriction == st.settings.free_communication_restriction) {
        done(out, 0, false);
        return;
    }
    done(out, pctl_set_custom_settings(&s), true);
}

void sync_exec(const SyncIntent *in, const SyncExecCtx *ctx, SyncOutcome *out)
{
    sync_outcome_init(out);
    if (!sync_intent_on_console(in->kind)) {
        refuse(out, SyncReason_UnknownEntity, 0);
        return;
    }
    if (sync_intent_needs_timer_writes(in->kind) && !ctx->remote_timer_writes) {
        refuse(out, SyncReason_TimerWritesDisabled, 0);
        return;
    }
    if (core_read_only()) {
        refuse(out, SyncReason_ReadOnly, NXM_RC_READ_ONLY);
        return;
    }
    switch (in->kind) {
    case SyncIntent_LimitDay:
    case SyncIntent_LimitUniform:
    case SyncIntent_LimitsWeek:
        exec_limits(in, ctx, out, in->mask);
        return;
    case SyncIntent_LimitToday: {
        if (ctx->weekday < 0 || ctx->weekday > 6) {
            refuse(out, SyncReason_Unsupported, 0);
            return;
        }
        SyncIntent today = *in;
        today.days[ctx->weekday] = in->minutes;
        exec_limits(&today, ctx, out, (uint8_t)(1u << ctx->weekday));
        return;
    }
    case SyncIntent_RemoveLimit: {
        SyncIntent none = *in;
        for (int d = 0; d < 7; d++) none.days[d] = NOLIMIT;
        exec_limits(&none, ctx, out, 0x7F);
        return;
    }
    case SyncIntent_Profile: {
        out->source = "remote_profile";
        SyncIntent prof = *in;
        if (!ctx->profile_days || !ctx->profile_days(ctx->ctx, in->profile, prof.days)) {
            refuse(out, SyncReason_NoSuchProfile, 0);
            return;
        }
        exec_limits(&prof, ctx, out, 0x7F);
        return;
    }
    case SyncIntent_ConsoleLock:
        exec_console_lock(in, ctx, out);
        return;
    case SyncIntent_BonusTime:
        exec_bonus(in, ctx, out);
        return;
    case SyncIntent_StopToday:
        exec_stop_today(ctx, out);
        return;
    case SyncIntent_Unlock: {
        out->change = SyncChange_Unlock;
        bool unlocked = false;
        if (R_SUCCEEDED(pctl_lock_state(NULL, &unlocked)) && unlocked) {
            done(out, 0, false);
            return;
        }
        done(out, pctl_unlock_restriction_temporarily(), true);
        return;
    }
    case SyncIntent_LockNow: {
        out->change = SyncChange_Relock;
        const Result rc = pctl_relock();
        if (R_SUCCEEDED(rc) && ctx->rec->relock_pending) {
            ctx->rec->relock_pending = false;
            out->records_changed = true;
        }
        done(out, rc, true);
        return;
    }
    case SyncIntent_Alarm:
        exec_alarm(in, ctx, out);
        return;
    case SyncIntent_BedtimeEnabled:
    case SyncIntent_Bedtime:
    case SyncIntent_BedtimeEnd:
        exec_bedtime(in, ctx, out);
        return;
    case SyncIntent_Level:
    case SyncIntent_Vr:
    case SyncIntent_Sns:
    case SyncIntent_Comm:
        exec_restrictions(in, out);
        return;
    default:
        refuse(out, SyncReason_UnknownEntity, 0);
        return;
    }
}

bool sync_exec_restore_extra(const SyncExecCtx *ctx, SyncOutcome *out)
{
    sync_outcome_init(out);
    out->source = "remote_restore_extra";
    SyncRecords *r = ctx->rec;
    if (r->extra_weekday < 0 || r->extra_weekday > 6) return false;
    if (ctx->today && !strcmp(r->extra_date, ctx->today)) return false;   // still the day it was added
    if (r->console_lock || core_read_only()) return false;                 // put back once these allow it
    PtState pt;
    pctl_play_timer_query(&pt);
    if (!pt.fw_supported || !pt.valid) return false;   // cannot tell now: next time
    const int wd = r->extra_weekday;
    if (pt.day_min[wd] != r->extra_value) {
        // Changed since: nothing to put back.
        r->extra_weekday = -1;
        r->extra_date[0] = '\0';
        out->records_changed = true;
        return false;
    }
    // As PlayGuard since main's #182 (pt_logic::restore_way): by itself only
    // what extra time raised, the limit coming back down. No more play today
    // and a "no limit" base wait for PlayGuard, which asks (or puts them back
    // itself when told to).
    if (r->extra_base == PT_DAY_NOLIMIT || r->extra_base >= r->extra_value) return false;
    // On the record's weekday again, not while the lower limit would stop the
    // game in progress: a later check puts it back.
    if (wd == ctx->weekday && !pt.alarm_only) {
        const int played = sync_played_today_min(&pt, wd);
        if (played > 0 && r->extra_base < played) return false;
    }
    uint16_t days[7];
    memcpy(days, pt.day_min, sizeof(days));
    days[wd] = r->extra_base;
    write_limits(ctx, out, &pt, days, NULL);
    if (out->applied) {
        r->extra_weekday = -1;
        r->extra_date[0] = '\0';
        out->records_changed = true;
    }
    return true;
}
