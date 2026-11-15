// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_apply.h"

#include <stdio.h>
#include <string.h>

#include "sync_conf.h"

#define NOLIMIT 0xFFFFu

static const char *const DAY_IDS[7] = { "limit_sun", "limit_mon", "limit_tue", "limit_wed",
                                        "limit_thu", "limit_fri", "limit_sat" };

// The payload as a C string, spaces around it dropped; false when too long.
static bool text(const char *payload, size_t len, char *out, size_t cap)
{
    while (len > 0 && (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n')) {
        payload++;
        len--;
    }
    while (len > 0 && (payload[len - 1] == ' ' || payload[len - 1] == '\t' || payload[len - 1] == '\r' ||
                       payload[len - 1] == '\n'))
        len--;
    if (len >= cap) return false;
    for (size_t i = 0; i < len; i++)
        if (payload[i] == '\0') return false;
    memcpy(out, payload, len);
    out[len] = '\0';
    return true;
}

// A limit: 0..1440, where 1440 is "no limit". A number with a decimal part
// (HA's number entity sends "90.0") is accepted when it is whole.
static SyncReason minutes(const char *s, uint16_t *out)
{
    char buf[16];
    const char *dot = strchr(s, '.');
    if (dot) {
        for (const char *p = dot + 1; *p; p++)
            if (*p != '0') return SyncReason_Invalid;
        const size_t n = (size_t)(dot - s);
        if (n == 0 || n >= sizeof(buf)) return SyncReason_Invalid;
        memcpy(buf, s, n);
        buf[n] = '\0';
        s = buf;
    }
    uint32_t v = 0;
    if (!bl_parse_uint(s, 0, 0xFFFFFFFFu, &v)) return SyncReason_Invalid;
    if (v > SYNC_NO_LIMIT_MIN) return SyncReason_OutOfRange;
    *out = v == SYNC_NO_LIMIT_MIN ? (uint16_t)NOLIMIT : (uint16_t)v;
    return SyncReason_None;
}

static SyncReason on_off(const char *s, bool *out)
{
    if (!strcmp(s, "ON") || !strcmp(s, "on")) *out = true;
    else if (!strcmp(s, "OFF") || !strcmp(s, "off")) *out = false;
    else return SyncReason_Invalid;
    return SyncReason_None;
}

// "HH:MM" or "HH:MM:SS" (what Home Assistant's time entity sends).
static SyncReason clock_time(const char *s, uint8_t *hour, uint8_t *minute)
{
    const size_t n = strlen(s);
    if (n != 5 && n != 8) return SyncReason_Invalid;
    for (size_t i = 0; i < n; i++) {
        const bool colon = i == 2 || i == 5;
        if (colon ? s[i] != ':' : (s[i] < '0' || s[i] > '9')) return SyncReason_Invalid;
    }
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return SyncReason_OutOfRange;
    if (n == 8 && ((s[6] - '0') * 10 + (s[7] - '0')) > 59) return SyncReason_OutOfRange;
    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    return SyncReason_None;
}

static bool press(const char *s)
{
    return !strcmp(s, "PRESS") || !strcmp(s, "press") || !strcmp(s, "1") || !strcmp(s, "ON");
}

SyncReason sync_apply_parse(const char *entity, const char *payload, size_t len, SyncIntent *out)
{
    memset(out, 0, sizeof(*out));
    char s[SYNC_PAYLOAD_MAX];
    if (!entity) return SyncReason_UnknownEntity;
    if (!text(payload, len, s, sizeof(s))) return SyncReason_Invalid;

    for (int d = 0; d < 7; d++) {
        if (!strcmp(entity, DAY_IDS[d])) {
            out->kind = SyncIntent_LimitDay;
            out->day = d;
            const SyncReason r = minutes(s, &out->minutes);
            out->days[d] = out->minutes;
            out->mask = (uint8_t)(1u << d);
            return r;
        }
    }
    if (!strcmp(entity, "limit_uniform")) {
        out->kind = SyncIntent_LimitUniform;
        const SyncReason r = minutes(s, &out->minutes);
        for (int d = 0; d < 7; d++) out->days[d] = out->minutes;
        out->mask = 0x7F;
        return r;
    }
    if (!strcmp(entity, "max_screentime_today")) {
        out->kind = SyncIntent_LimitToday;
        return minutes(s, &out->minutes);
    }
    if (!strcmp(entity, "limits_week")) {
        // Seven comma-separated limits, Sunday first.
        out->kind = SyncIntent_LimitsWeek;
        out->mask = 0x7F;
        const char *p = s;
        for (int d = 0; d < 7; d++) {
            char part[16];
            const char *comma = strchr(p, ',');
            const size_t n = comma ? (size_t)(comma - p) : strlen(p);
            if ((d < 6) != (comma != NULL) || n == 0 || n >= sizeof(part)) return SyncReason_Invalid;
            memcpy(part, p, n);
            part[n] = '\0';
            size_t a = 0, b = n;
            while (a < b && part[a] == ' ') a++;
            while (b > a && part[b - 1] == ' ') b--;
            part[b] = '\0';
            const SyncReason r = minutes(part + a, &out->days[d]);
            if (r != SyncReason_None) return r;
            p = comma ? comma + 1 : p + n;
        }
        return SyncReason_None;
    }
    if (!strcmp(entity, "remove_limit")) {
        out->kind = SyncIntent_RemoveLimit;
        return press(s) ? SyncReason_None : SyncReason_Invalid;
    }
    if (!strcmp(entity, "console_lock")) {
        out->kind = SyncIntent_ConsoleLock;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "add_bonus_time") || !strcmp(entity, "add_bonus_time_15") ||
        !strcmp(entity, "add_bonus_time_30") || !strcmp(entity, "add_bonus_time_60")) {
        out->kind = SyncIntent_BonusTime;
        if (entity[14] == '_') {   // a button: the amount is in its name
            if (!press(s)) return SyncReason_Invalid;
            out->minutes = (uint16_t)(entity[15] == '1' ? 15 : entity[15] == '3' ? 30 : 60);
            return SyncReason_None;
        }
        uint32_t v = 0;
        if (!bl_parse_uint(s, 0, 0xFFFFFFFFu, &v)) return SyncReason_Invalid;
        if (v < 5 || v > 180) return SyncReason_OutOfRange;
        out->minutes = (uint16_t)v;
        return SyncReason_None;
    }
    if (!strcmp(entity, "stop_today")) {
        out->kind = SyncIntent_StopToday;
        return press(s) ? SyncReason_None : SyncReason_Invalid;
    }
    if (!strcmp(entity, "parental_controls")) {
        // The lock: LOCK is always allowed, as on the console.
        if (!strcmp(s, "LOCK")) out->kind = SyncIntent_LockNow;
        else if (!strcmp(s, "UNLOCK")) out->kind = SyncIntent_Unlock;
        else return SyncReason_Invalid;
        out->on = out->kind == SyncIntent_Unlock;
        return SyncReason_None;
    }
    // The switch and the button the lock replaced (an integration that
    // still sends them).
    if (!strcmp(entity, "unlocked")) {
        const SyncReason r = on_off(s, &out->on);
        out->kind = out->on ? SyncIntent_Unlock : SyncIntent_LockNow;
        return r;
    }
    if (!strcmp(entity, "lock_now")) {
        out->kind = SyncIntent_LockNow;
        return press(s) ? SyncReason_None : SyncReason_Invalid;
    }
    if (!strcmp(entity, "play_timer_alarm")) {
        out->kind = SyncIntent_Alarm;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "bedtime_enabled")) {
        out->kind = SyncIntent_BedtimeEnabled;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "bedtime_alarm")) {
        out->kind = SyncIntent_Bedtime;
        const SyncReason r = clock_time(s, &out->hour, &out->minute);
        if (r != SyncReason_None) return r;
        const int t = out->hour * 60 + out->minute;
        return t >= 16 * 60 ? SyncReason_None : SyncReason_OutOfRange;   // 16:00 to 23:59
    }
    if (!strcmp(entity, "bedtime_end_time")) {
        out->kind = SyncIntent_BedtimeEnd;
        const SyncReason r = clock_time(s, &out->hour, &out->minute);
        if (r != SyncReason_None) return r;
        const int t = out->hour * 60 + out->minute;
        return t >= 5 * 60 && t <= 9 * 60 ? SyncReason_None : SyncReason_OutOfRange;   // 05:00 to 09:00
    }
    if (!strcmp(entity, "profile")) {
        out->kind = SyncIntent_Profile;
        const size_t n = strlen(s);
        if (n == 0 || n >= sizeof(out->profile)) return SyncReason_Invalid;
        memcpy(out->profile, s, n + 1);
        return SyncReason_None;
    }
    if (!strcmp(entity, "restriction_level")) {
        out->kind = SyncIntent_Level;
        static const char *const names[5] = { "none", "custom", "young_child", "child", "teen" };
        for (uint32_t i = 0; i < 5; i++)
            if (!strcmp(s, names[i])) {
                out->level = i;
                return SyncReason_None;
            }
        // The select's option for "none" (sync_entities.c), sent as is by a
        // client that does not apply its command template.
        if (!strcmp(s, "unrestricted")) {
            out->level = 0;
            return SyncReason_None;
        }
        return SyncReason_Invalid;
    }
    if (!strcmp(entity, "vr_restricted")) {
        out->kind = SyncIntent_Vr;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "sns_post_restricted")) {
        out->kind = SyncIntent_Sns;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "free_communication_restricted")) {
        out->kind = SyncIntent_Comm;
        return on_off(s, &out->on);
    }
    if (!strcmp(entity, "sync_now")) {
        out->kind = SyncIntent_SyncNow;
        return press(s) ? SyncReason_None : SyncReason_Invalid;
    }
    if (!strcmp(entity, "export_report")) {
        out->kind = SyncIntent_ExportReport;
        return press(s) ? SyncReason_None : SyncReason_Invalid;
    }
    if (!strcmp(entity, "discovery")) {
        out->kind = SyncIntent_Discovery;
        return on_off(s, &out->on);
    }
    return SyncReason_UnknownEntity;
}

bool sync_intent_needs_timer_writes(SyncIntentKind kind)
{
    switch (kind) {
    case SyncIntent_LimitDay:
    case SyncIntent_LimitUniform:
    case SyncIntent_LimitsWeek:
    case SyncIntent_LimitToday:
    case SyncIntent_RemoveLimit:
    case SyncIntent_ConsoleLock:
    case SyncIntent_BonusTime:
    case SyncIntent_StopToday:
    case SyncIntent_Unlock:
    case SyncIntent_Alarm:
    case SyncIntent_BedtimeEnabled:
    case SyncIntent_Bedtime:
    case SyncIntent_BedtimeEnd:
    case SyncIntent_Profile:
        return true;
    default:
        return false;
    }
}

bool sync_intent_on_console(SyncIntentKind kind)
{
    return kind != SyncIntent_None && kind != SyncIntent_SyncNow && kind != SyncIntent_ExportReport &&
           kind != SyncIntent_Discovery;
}

bool sync_intent_harmless(SyncIntentKind kind)
{
    return kind == SyncIntent_SyncNow || kind == SyncIntent_ExportReport;
}

const char *sync_reason_name(SyncReason r)
{
    switch (r) {
        case SyncReason_None: return "";
        case SyncReason_UnknownEntity: return "unknown_entity";
        case SyncReason_Invalid: return "invalid";
        case SyncReason_OutOfRange: return "out_of_range";
        case SyncReason_PolicyOff: return "policy_off";
        case SyncReason_TimerWritesDisabled: return "timer_writes_disabled";
        case SyncReason_ReadOnly: return "read_only";
        case SyncReason_NotConfirmed: return "not_confirmed";
        case SyncReason_Gated: return "gated";
        case SyncReason_UnlockFailed: return "unlock_failed";
        case SyncReason_NotCustom: return "not_custom";
        case SyncReason_NoSuchProfile: return "no_such_profile";
        case SyncReason_NoLimitToday: return "no_limit_today";
        case SyncReason_ConsoleLocked: return "console_locked";
        case SyncReason_Unsupported: return "unsupported";
        case SyncReason_BedtimeOff: return "bedtime_off";
        case SyncReason_PctlError: return "pctl_error";
        case SyncReason_Waiting: return "waiting_for_confirmation";
        case SyncReason_Busy: return "busy";
    }
    return "unknown";
}
