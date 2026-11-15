// PlayGuard — console-free part of the service layer (see pure.h).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "pure.h"
#include "fw_policy.h"
#include "pctl_ops.h"
#include "core/sysinfo.h"
#include <string.h>

// The block as bytes, whatever the host's byte order: byte 2i is the low
// half of u16 i, as on the console.
static u8 pt_byte(const u16 c[PT_U16_COUNT], int at)
{
    return (u8)(at & 1 ? c[at / 2] >> 8 : c[at / 2] & 0xFF);
}

static void pt_set_byte(u16 c[PT_U16_COUNT], int at, u8 v)
{
    c[at / 2] = at & 1 ? (u16)((c[at / 2] & 0x00FF) | (v << 8)) : (u16)((c[at / 2] & 0xFF00) | v);
}

// Byte offsets inside a rule (pure.h).
enum { PT_DAILY = 4, PT_DAY0 = 12, PT_DAYSZ = 8, PT_BED_ON = 0, PT_BED_H, PT_BED_M, PT_END_H, PT_END_M, PT_LIMIT_ON };

static int pt_day(int n) { return PT_DAY0 + PT_DAYSZ * n; }

bool pt_suspends(const u16 c[PT_U16_COUNT]) { return pt_byte(c, PT_MODE_RESTRICTION) != 0; }
bool pt_daily(const u16 c[PT_U16_COUNT]) { return pt_byte(c, PT_MODE_TIMER) == 0; }

// The rule the console applies on day n: the daily one in DAILY mode.
static int pt_rule(const u16 c[PT_U16_COUNT], int n) { return pt_daily(c) ? PT_DAILY : pt_day(n); }

static bool pt_any_bedtime(const u16 c[PT_U16_COUNT])
{
    for (int n = 0; n < 7; n++)
        if (pt_byte(c, pt_rule(c, n) + PT_BED_ON)) return true;
    return false;
}

bool pt_configured(const u16 c[PT_U16_COUNT])
{
    for (int n = 0; n < 7; n++)
        if (pt_byte(c, pt_rule(c, n) + PT_LIMIT_ON)) return true;
    return pt_any_bedtime(c);
}

bool pt_set_suspends(u16 c[PT_U16_COUNT], bool suspends)
{
    if (!pt_configured(c)) return false;
    pt_set_byte(c, PT_MODE_RESTRICTION, suspends ? 1 : 0);
    return true;
}

// The limit flag alone: the low half of [+1] is the bedtime's allowed-again
// minute.
static bool pt_has_limit(const u16 c[PT_U16_COUNT], int n)
{
    return pt_byte(c, pt_day(n) + PT_LIMIT_ON) != 0;
}

void pt_decode(const u16 c[PT_U16_COUNT], u16 days_min[7])
{
    for (int n = 0; n < 7; n++) {
        const int r = pt_rule(c, n);
        days_min[n] = pt_byte(c, r + PT_LIMIT_ON) ? (u16)(pt_byte(c, r + 6) | (pt_byte(c, r + 7) << 8))
                                                  : PT_DAY_NOLIMIT;
    }
}

// A DAILY block about to change per day: timerMode set to
// EACH_DAY_OF_THE_WEEK, the seven days given the daily rule (the console
// ignored what they held), so the block says the same thing in the mode
// PlayGuard writes. A daily rule with nothing on gives all-zero days instead,
// the "off" days seen working (the timer-off block is DAILY). The daily rule
// itself is kept.
static void pt_to_each_day(u16 c[PT_U16_COUNT])
{
    if (!pt_daily(c)) return;
    const bool on = pt_byte(c, PT_DAILY + PT_BED_ON) || pt_byte(c, PT_DAILY + PT_LIMIT_ON);
    for (int n = 0; n < 7; n++)
        for (int i = 0; i < PT_DAYSZ; i++) pt_set_byte(c, pt_day(n) + i, on ? pt_byte(c, PT_DAILY + i) : 0);
    pt_set_byte(c, PT_MODE_TIMER, 1);
}

// One 8-byte rule at byte `d` (a day, or the header's): see pure.h.
static bool pt_rule_plausible(const u16 c[PT_U16_COUNT], int d)
{
    const u16 minutes = (u16)(pt_byte(c, d + 6) | (pt_byte(c, d + 7) << 8));
    return pt_byte(c, d + PT_BED_ON) <= 1 && pt_byte(c, d + PT_BED_H) < 24 && pt_byte(c, d + PT_BED_M) < 60 &&
           pt_byte(c, d + PT_END_H) < 24 && pt_byte(c, d + PT_END_M) < 60 && pt_byte(c, d + PT_LIMIT_ON) <= 1 &&
           (minutes <= 1440 || minutes == PT_DAY_NOLIMIT);
}

bool pt_plausible(const u16 c[PT_U16_COUNT])
{
    // The four mode bytes (00..03) are not decoded: an unseen companion-app
    // setting may live there, so any value passes.
    if (!pt_rule_plausible(c, 4)) return false;
    for (int n = 0; n < 7; n++)
        if (!pt_rule_plausible(c, pt_day(n))) return false;
    return true;
}

void pt_encode(u16 c[PT_U16_COUNT], const u16 days_min[7])
{
    bool any = false;
    for (int n = 0; n < 7; n++) if (days_min[n] != PT_DAY_NOLIMIT) any = true;
    if (!any && !pt_any_bedtime(c)) {
        memset(c, 0, PT_U16_COUNT * sizeof(u16));
        return;
    }
    if (pt_daily(c)) {
        u16 now[7];
        pt_decode(c, now);
        if (memcmp(now, days_min, sizeof(now)) == 0) return;   // unchanged: left exactly as read
        pt_to_each_day(c);
    }
    if (c[0] == 0) {
        c[0] = 0x0101;
        c[1] = 0x0001;
    }
    for (int n = 0; n < 7; n++) {
        u16 *g = &c[7 + 4 * n];
        const bool had = pt_has_limit(c, n);
        // Its bedtime's times share [+0] and [+1] with the limit flag.
        const bool bed = pt_byte(c, pt_day(n) + PT_BED_ON) != 0;
        if (days_min[n] == PT_DAY_NOLIMIT) {
            if (had && bed) {
                pt_set_byte(c, pt_day(n) + PT_LIMIT_ON, 0);
                g[2] = 0;
            } else if (had) {
                g[0] = g[1] = g[2] = 0;
            }
            continue;
        }
        if (!had && bed) {
            pt_set_byte(c, pt_day(n) + PT_LIMIT_ON, 1);
        } else if (!had) {
            g[0] = 0x0600;
            g[1] = 0x0100;
        }
        g[2] = days_min[n];
    }
}

bool pt_days_ok(const u16 days_min[7])
{
    for (int n = 0; n < 7; n++)
        if (days_min[n] != PT_DAY_NOLIMIT && days_min[n] > 1440) return false;
    return true;
}

bool pt_bedtime_ok(const PtBedtime *b)
{
    if (!b->on) return true;
    const unsigned end = b->end_hour * 60u + b->end_minute;
    return b->hour >= 16 && b->hour <= 23 && b->minute < 60 && b->end_minute < 60 &&
           end >= 5 * 60 && end <= 9 * 60;
}

void pt_bedtime_decode(const u16 c[PT_U16_COUNT], PtBedtime out[7])
{
    for (int n = 0; n < 7; n++) {
        const int d = pt_rule(c, n);
        out[n].on         = pt_byte(c, d + PT_BED_ON) != 0;
        out[n].hour       = pt_byte(c, d + PT_BED_H);
        out[n].minute     = pt_byte(c, d + PT_BED_M);
        out[n].end_hour   = pt_byte(c, d + PT_END_H);
        out[n].end_minute = pt_byte(c, d + PT_END_M);
    }
}

// Whether two bedtimes are the same (the times only matter while it is on).
static bool pt_bedtime_same(const PtBedtime *a, const PtBedtime *b)
{
    return a->on == b->on && (!a->on || (a->hour == b->hour && a->minute == b->minute &&
                                         a->end_hour == b->end_hour && a->end_minute == b->end_minute));
}

void pt_bedtime_encode(u16 c[PT_U16_COUNT], const PtBedtime in[7])
{
    PtBedtime now[7];
    pt_bedtime_decode(c, now);
    bool same = true;
    for (int n = 0; n < 7; n++) if (!pt_bedtime_same(&in[n], &now[n])) same = false;
    if (same && pt_daily(c)) return;   // unchanged: left exactly as read
    pt_to_each_day(c);
    for (int n = 0; n < 7; n++) {
        const int d = pt_day(n);
        if (pt_bedtime_same(&in[n], &now[n])) continue;   // unchanged: left exactly as read
        pt_set_byte(c, d + PT_BED_ON, in[n].on ? 1 : 0);
        pt_set_byte(c, d + PT_BED_H, in[n].on ? in[n].hour : 0);
        pt_set_byte(c, d + PT_BED_M, in[n].on ? in[n].minute : 0);
        if (in[n].on) {
            pt_set_byte(c, d + PT_END_H, in[n].end_hour);
            pt_set_byte(c, d + PT_END_M, in[n].end_minute);
        }
    }
    u16 days[7];
    pt_decode(c, days);
    bool any = pt_any_bedtime(c);
    for (int n = 0; n < 7; n++) if (days[n] != PT_DAY_NOLIMIT) any = true;
    if (!any) {
        memset(c, 0, PT_U16_COUNT * sizeof(u16));
    } else if (c[0] == 0) {
        c[0] = 0x0101;
        c[1] = 0x0001;
    }
}

const char *pctl_rating_org_name(u32 org)
{
    // nn::ns::RatingOrganization
    static const char *names[] = {
        "CERO", "GRAC", "GSRMR", "ESRB", "ClassInd", "USK", "PEGI",
        "PEGI Portugal", "PEGI BBFC", "Russian", "ACB", "OFLC", "IARC Generic",
    };
    return org < sizeof(names) / sizeof(names[0]) ? names[org] : "?";
}

SysCompat sysinfo_compat(const SysInfo *info)
{
    if (!info->is_atmosphere)                       return SysCompat_NotAtmosphere;
    if (info->hos_version < PCTL_FW_MIN_PLAYTIMER)  return SysCompat_PlayTimerUnsupported;
    if (info->hos_version > PCTL_FW_TESTED_MAX)     return SysCompat_UntestedNewer;
    return SysCompat_Ok;
}

bool pin_is_weak(const char *pin, size_t n)
{
    if (!pin || n < 4) return false;
    bool same = true, up = true, down = true, pairs = n % 2 == 0;
    for (size_t i = 1; i < n; i++) {
        same = same && pin[i] == pin[0];
        up   = up && pin[i] == pin[i - 1] + 1;
        down = down && pin[i] == pin[i - 1] - 1;
        if (pairs && i >= 2) pairs = pin[i] == pin[i - 2];
    }
    if (same || up || down || pairs) return true;
    return n == 4 && (memcmp(pin, "2580", 4) == 0 || memcmp(pin, "0852", 4) == 0);
}
