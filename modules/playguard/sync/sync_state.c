// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_state.h"

#include <stdio.h>
#include <string.h>

#include "bl_json.h"
#include "sync_titles.h"

#define NOLIMIT PT_DAY_NOLIMIT

static const char *const DAY_KEYS[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

int sync_played_today_min(const PtState *pt, int weekday)
{
    if (!pt->valid || !pt->enabled_valid || !pt->enabled || weekday < 0 || weekday > 6) return -1;
    const uint16_t limit = pt->day_min[weekday];
    if (limit == PT_DAY_NOLIMIT) return -1;
    if (pt->restricted_valid && pt->restricted) return limit;
    if (!pt->remaining_valid || pt->remaining_ns == 0) return -1;
    const uint64_t left = pt->remaining_ns / 60000000000ULL;
    return left >= limit ? 0 : (int)(limit - left);
}

const char *sync_level_name(uint32_t level)
{
    static const char *const names[5] = { "none", "custom", "young_child", "child", "teen" };
    return level < 5 ? names[level] : "";
}

// A limit on the wire: 1440 for "no limit".
static int64_t wire_limit(uint16_t v)
{
    return v == NOLIMIT ? 1440 : v;
}

static void hhmm(BlJson *j, const char *key, bool ok, uint8_t hour, uint8_t minute)
{
    bl_json_key(j, key);
    if (!ok) {
        bl_json_null(j);
        return;
    }
    char t[12];
    snprintf(t, sizeof(t), "%02u:%02u:00", hour % 24u, minute % 60u);
    bl_json_str(j, t);
}

static void version(BlJson *j, const char *key, const SysInfo *sys, bool ams)
{
    bl_json_key(j, key);
    char v[24];
    if (!sys || (ams && !sys->ams_valid) || (!ams && !sys->hos_version)) {
        bl_json_null(j);
        return;
    }
    if (ams) snprintf(v, sizeof(v), "%u.%u.%u", sys->ams_major, sys->ams_minor, sys->ams_micro);
    else snprintf(v, sizeof(v), "%u.%u.%u", (unsigned)((sys->hos_version >> 16) & 0xFF),
                  (unsigned)((sys->hos_version >> 8) & 0xFF), (unsigned)(sys->hos_version & 0xFF));
    bl_json_str(j, v);
}

static void console(BlJson *j, const SyncSnapshot *s)
{
    // This system's id and name (sync_conf_device_id): an emuMMC is a device of its own.
    char id[SYNC_ID_LEN + 1] = "", name[SYNC_DEVICE_NAME_MAX] = "";
    if (s->conf) {
        sync_conf_device_id(s->conf, id);
        // Named when sync.conf names it, by its nickname, or on an emuMMC; else
        // null (the integration says "Nintendo Switch" itself).
        const bool nicknamed = s->conf->publish_hardware && s->nickname && s->nickname[0];
        if (s->conf->console_name[0] || nicknamed || s->conf->emummc)
            sync_conf_device_name(s->conf, s->nickname, name, sizeof(name));
    }
    bl_json_key(j, "console");
    bl_json_obj(j);
    bl_json_kstr_or_null(j, "id", id[0] ? id : NULL);
    bl_json_kstr_or_null(j, "name", name[0] ? name : NULL);
    // The system's versions are versions and hardware (publish_hardware).
    const SysInfo *versions = !s->conf || s->conf->publish_hardware ? s->sys : NULL;
    version(j, "firmware", versions, false);
    version(j, "atmosphere", versions, true);
    bl_json_kstr_or_null(j, "app_version", s->app_version);
    bl_json_kstr_or_null(j, "agent_version", s->agent_version);
    bl_json_kbool_or_null(j, "emummc", s->sys && s->sys->emummc_valid, s->sys && s->sys->emummc);
    bl_json_kbool(j, "read_only", s->read_only);
    bl_json_obj_end(j);
}

static void controls(BlJson *j, const SyncSnapshot *s)
{
    const PctlStatus *st = s->status;
    bl_json_key(j, "controls");
    bl_json_obj(j);
    bl_json_kbool_or_null(j, "enabled", st && st->restriction_enabled_ok, st && st->restriction_enabled);
    bl_json_kbool_or_null(j, "temp_unlocked", st && st->temp_unlocked_ok, st && st->temp_unlocked);
    bl_json_kbool_or_null(j, "pin_set", st && st->pin_length_ok, st && st->pin_length > 0);
    bl_json_key(j, "level");
    if (st && st->safety_level_ok && *sync_level_name(st->safety_level)) bl_json_str(j, sync_level_name(st->safety_level));
    else bl_json_null(j);
    bl_json_kint_or_null(j, "rating_age", st && st->settings_ok, st ? st->settings.rating_age : 0);
    bl_json_kbool_or_null(j, "sns_post_restricted", st && st->settings_ok, st && st->settings.sns_post_restriction);
    bl_json_kbool_or_null(j, "free_communication_restricted", st && st->settings_ok,
                            st && st->settings.free_communication_restriction);
    bl_json_kbool_or_null(j, "vr_restricted", st && st->stereo_vision_ok, st && st->stereo_vision_restricted);
    bl_json_kint_or_null(j, "rating_org", st && st->rating_org_ok, st ? st->rating_org : 0);
    bl_json_kbool_or_null(j, "companion_linked", st && st->pairing_active_ok, st && st->pairing_active);
    bl_json_obj_end(j);
}

const char *sync_blocked_by(const SyncSnapshot *s)
{
    const PtState *pt = s->timer;
    if (!pt || !pt->fw_supported || !pt->restricted_valid || !pt->restricted) return NULL;
    if (pt->bedtime_valid && pt->bedtime_enabled && s->local_min_ok) {
        const int start = pt->bedtime_hour * 60 + pt->bedtime_minute;
        const int wd = s->weekday;
        int end = -1;
        if (pt->bedtime_reset_valid) end = pt->bedtime_reset_hour * 60 + pt->bedtime_reset_minute;
        else if (pt->valid && wd >= 0 && wd <= 6 && pt->bed[wd].on) end = pt->bed[wd].end_hour * 60 + pt->bed[wd].end_minute;
        if (s->local_min >= start || s->local_min < end) return "bedtime";
    }
    return "limit";
}

bool sync_remaining_min(const SyncSnapshot *s, int64_t *left)
{
    const PtState *pt = s->timer;
    const int wd = s->weekday;
    if (!pt || !pt->fw_supported || !pt->valid || wd < 0 || wd > 6) return false;
    if (!pt->enabled_valid || !pt->enabled || pt->day_min[wd] == NOLIMIT) return false;
    // The console's count, or the whole limit while no game has been counted
    // yet (1454 reads 0 until then).
    if (pt->restricted_valid && pt->restricted) {
        *left = 0;
        return true;
    }
    if (!pt->remaining_valid) return false;
    *left = pt->remaining_ns ? (int64_t)((pt->remaining_ns + 30000000000ULL) / 60000000000ULL) : pt->day_min[wd];
    return true;
}

static void timer(BlJson *j, const SyncSnapshot *s)
{
    const PtState *pt = s->timer;
    const bool valid = pt && pt->fw_supported && pt->valid;
    const int wd = s->weekday;
    const bool today_ok = valid && wd >= 0 && wd <= 6;
    const bool enabled_ok = pt && pt->fw_supported && pt->enabled_valid;

    bl_json_key(j, "timer");
    bl_json_obj(j);
    bl_json_kbool(j, "supported", pt ? pt->fw_supported : false);
    bl_json_kbool_or_null(j, "enabled", enabled_ok, enabled_ok && pt->enabled);
    bl_json_kbool_or_null(j, "limit_reached", pt && pt->fw_supported && pt->restricted_valid,
                            pt && pt->restricted);
    bl_json_kstr_or_null(j, "blocked_by", sync_blocked_by(s));

    bl_json_key(j, "limits");
    bl_json_obj(j);
    for (int d = 0; d < 7; d++) bl_json_kint_or_null(j, DAY_KEYS[d], valid, valid ? wire_limit(pt->day_min[d]) : 0);
    bl_json_obj_end(j);

    bl_json_key(j, "limits_min");
    if (valid) {
        bl_json_arr(j);
        for (int d = 0; d < 7; d++) bl_json_int(j, wire_limit(pt->day_min[d]));
        bl_json_arr_end(j);
    } else {
        bl_json_null(j);
    }

    bool uniform = valid;
    for (int d = 1; d < 7 && uniform; d++) uniform = pt->day_min[d] == pt->day_min[0];
    bl_json_kint_or_null(j, "uniform_min", uniform, uniform ? wire_limit(pt->day_min[0]) : 0);

    const uint16_t limit = today_ok ? pt->day_min[wd] : NOLIMIT;
    bl_json_kint_or_null(j, "limit_today_min", today_ok, wire_limit(limit));

    int64_t left = 0;
    const bool left_ok = sync_remaining_min(s, &left);
    bl_json_kint_or_null(j, "remaining_min", left_ok, left);
    const int played = pt ? sync_played_today_min(pt, wd) : -1;
    bl_json_kint_or_null(j, "used_min", played >= 0, played);
    bl_json_kint_or_null(j, "spent_raw_min", s->spent_ok, (int64_t)(s->spent_ns / 60000000000ULL));
    bl_json_kbool_or_null(j, "alarm_on", pt && pt->fw_supported && pt->alarm_disabled_valid,
                            pt && !pt->alarm_disabled);

    bl_json_key(j, "bedtime");
    bl_json_obj(j);
    const bool bed_ok = pt && pt->fw_supported && pt->bedtime_valid;
    bl_json_kbool_or_null(j, "enabled", bed_ok, bed_ok && pt->bedtime_enabled);
    hhmm(j, "start", bed_ok && pt->bedtime_enabled, bed_ok ? pt->bedtime_hour : 0, bed_ok ? pt->bedtime_minute : 0);
    // When play is allowed again: the console's own answer (20.0.0+), else
    // what the block holds for today.
    if (pt && pt->fw_supported && pt->bedtime_reset_valid) {
        hhmm(j, "end", true, pt->bedtime_reset_hour, pt->bedtime_reset_minute);
    } else if (today_ok && pt->bed[wd].on) {
        hhmm(j, "end", true, pt->bed[wd].end_hour, pt->bed[wd].end_minute);
    } else {
        bl_json_knull(j, "end");
    }
    bl_json_obj_end(j);

    // Extra time granted today and still on today's limit.
    int64_t extended = 0;
    const SyncRecords *r = s->records;
    if (r && today_ok && r->extra_weekday == wd && s->local_date && !strcmp(r->extra_date, s->local_date) &&
        pt->day_min[wd] == r->extra_value && r->extra_value > r->extra_base && r->extra_base != NOLIMIT)
        extended = r->extra_value - r->extra_base;
    bl_json_kint_or_null(j, "extended_today_min", today_ok, extended);
    bl_json_kbool(j, "console_locked", r && r->console_lock);
    bl_json_kstr_or_null(j, "profile", s->profile_now);
    bl_json_obj_end(j);
}

static void activity_today(BlJson *j, const SyncSnapshot *s)
{
    // Play time (publish_activity): null while it is not published.
    const bool on = !s->conf || s->conf->publish_activity;
    const uint64_t playing = on ? s->now_playing : 0;
    bl_json_key(j, "activity_today");
    bl_json_obj(j);
    bl_json_kint_or_null(j, "used_min", on && s->activity_ok, (s->activity_s + 30) / 60);
    // What the console is doing: a game, a homebrew, PlayGuard, the HOME
    // menu, the HOME menu with a game waiting (sync_now_name).
    bl_json_kstr_or_null(j, "now", on ? sync_now_name(playing, s->now_playing_kind, s->now) : NULL);
    bl_json_key(j, "now_playing");
    bl_json_obj(j);
    bl_json_key(j, "app_id");
    if (playing) bl_json_hex64(j, playing);
    else bl_json_null(j);
    bl_json_kstr_or_null(j, "name", playing ? s->now_playing_name : NULL);
    bl_json_kint_or_null(j, "since", playing && s->now_playing_since, (int64_t)s->now_playing_since);
    bl_json_kstr_or_null(j, "kind", playing ? sync_title_kind_label(s->now_playing_kind) : NULL);
    bl_json_obj_end(j);
    bl_json_obj_end(j);
}

static void link(BlJson *j, const SyncSnapshot *s)
{
    bl_json_key(j, "link");
    bl_json_obj(j);
    bl_json_kstr(j, "policy", sync_policy_name(s->conf ? s->conf->policy : SyncPolicy_Ask));
    bl_json_kbool(j, "remote_timer_writes", s->conf && s->conf->remote_timer_writes);
    bl_json_kbool(j, "ha_discovery", s->conf && s->conf->ha_discovery);
    bl_json_kbool(j, "agent", s->agent);
    bl_json_kstr_or_null(j, "last_result", s->last_result);
    // What is published beside the parental controls (sync.conf's publish_*).
    bl_json_key(j, "publishes");
    bl_json_obj(j);
    for (int p = SyncPublish_Controls + 1; p < SyncPublish_Count; p++)
        bl_json_kbool(j, sync_publish_name((SyncPublish)p), !s->conf || sync_conf_publishes(s->conf, (SyncPublish)p));
    bl_json_obj_end(j);
    bl_json_obj_end(j);
}

size_t sync_state_build(const SyncSnapshot *s, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kstr(&j, "source", s->source ? s->source : "app");
    bl_json_kint(&j, "ts", (int64_t)s->ts);
    bl_json_kstr_or_null(&j, "local_date", s->local_date);
    bl_json_kint_or_null(&j, "weekday", s->weekday >= 0 && s->weekday <= 6, s->weekday);
    bl_json_kbool_or_null(&j, "clock_accurate", s->clock_accurate_ok, s->clock_accurate);
    console(&j, s);
    controls(&j, s);
    timer(&j, s);
    activity_today(&j, s);
    link(&j, s);
    bl_json_key(&j, "profiles");
    bl_json_arr(&j);
    for (size_t i = 0; s->profiles && i < s->n_profiles; i++) bl_json_str(&j, s->profiles[i]);
    bl_json_arr_end(&j);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}

static void uid_hex(BlJson *j, const uint64_t uid[2])
{
    char t[33];
    snprintf(t, sizeof(t), "%016llX%016llX", (unsigned long long)uid[0], (unsigned long long)uid[1]);
    bl_json_str(j, t);
}

size_t sync_activity_build(const SyncActivity *a, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kstr(&j, "source", a->source ? a->source : "app");
    bl_json_kint(&j, "ts", (int64_t)a->ts);
    bl_json_kstr_or_null(&j, "local_date", a->local_date);
    bl_json_kbool(&j, "final", a->final);
    uint64_t total = a->homebrew_s;
    for (size_t i = 0; i < a->n_apps; i++) total += a->apps[i].seconds;
    bl_json_kint(&j, "total_s", (int64_t)total);
    bl_json_kint(&j, "total_min", (int64_t)((total + 30) / 60));
    // The homebrews' time: in the total, not in per_app.
    bl_json_kint(&j, "homebrew_s", (int64_t)a->homebrew_s);
    bl_json_kint(&j, "homebrew_min", (int64_t)((a->homebrew_s + 30) / 60));
    bl_json_key(&j, "per_app");
    bl_json_arr(&j);
    for (size_t i = 0; i < a->n_apps; i++) {
        if (!a->apps[i].seconds) continue;
        bl_json_obj(&j);
        bl_json_key(&j, "app_id");
        bl_json_hex64(&j, a->apps[i].app_id);
        bl_json_kint(&j, "s", a->apps[i].seconds);
        bl_json_kint(&j, "min", (a->apps[i].seconds + 30) / 60);
        bl_json_obj_end(&j);
    }
    bl_json_arr_end(&j);
    bl_json_key(&j, "per_account");
    if (a->accounts) {
        bl_json_arr(&j);
        for (size_t i = 0; i < a->n_accounts; i++) {
            bl_json_obj(&j);
            bl_json_key(&j, "uid");
            uid_hex(&j, a->accounts[i].uid);
            bl_json_kint(&j, "s", a->accounts[i].seconds);
            bl_json_kint(&j, "min", (a->accounts[i].seconds + 30) / 60);
            bl_json_obj_end(&j);
        }
        bl_json_arr_end(&j);
    } else {
        bl_json_null(&j);
    }
    if (!a->final) {
        bl_json_key(&j, "now_playing");
        bl_json_obj(&j);
        bl_json_key(&j, "app_id");
        if (a->now_playing) bl_json_hex64(&j, a->now_playing);
        else bl_json_null(&j);
        bl_json_kint_or_null(&j, "since", a->now_playing && a->now_playing_since, (int64_t)a->now_playing_since);
        bl_json_kstr_or_null(&j, "kind", a->now_playing ? sync_title_kind_label(a->now_playing_kind) : NULL);
        bl_json_obj_end(&j);
    }
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}

size_t sync_names_build(uint64_t ts, const SyncAppName *apps, size_t n_apps, const SyncAccountName *accounts,
                        size_t n_accounts, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kint(&j, "ts", (int64_t)ts);
    bl_json_key(&j, "games");
    bl_json_obj(&j);
    for (size_t i = 0; i < n_apps; i++) {
        if (!apps[i].name || !*apps[i].name) continue;
        char key[17];
        snprintf(key, sizeof(key), "%016llX", (unsigned long long)apps[i].app_id);
        bl_json_kstr(&j, key, apps[i].name);
    }
    bl_json_obj_end(&j);
    bl_json_key(&j, "accounts");
    bl_json_obj(&j);
    for (size_t i = 0; i < n_accounts; i++) {
        char key[33];
        snprintf(key, sizeof(key), "%016llX%016llX", (unsigned long long)accounts[i].uid[0],
                 (unsigned long long)accounts[i].uid[1]);
        bl_json_kstr(&j, key, accounts[i].nickname ? accounts[i].nickname : "");
    }
    bl_json_obj_end(&j);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}

size_t sync_capabilities_build(const char *sw_version, bool playguard, const SyncRelayModule *relayed,
                               size_t n_relayed, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_CAPABILITIES_SCHEMA);
    bl_json_kstr(&j, "platform", SYNC_PLATFORM);
    bl_json_key(&j, "modules");
    bl_json_obj(&j);
    if (playguard) {
        bl_json_key(&j, "playguard");
        bl_json_obj(&j);
        bl_json_kint(&j, "schema", SYNC_SCHEMA);
        bl_json_kstr_or_null(&j, "version", sw_version && *sw_version ? sw_version : NULL);
        bl_json_obj_end(&j);
    }
    for (size_t i = 0; relayed && i < n_relayed; i++) {
        bl_json_key(&j, relayed[i].name);
        bl_json_obj(&j);
        bl_json_kstr_or_null(&j, "version", relayed[i].version);
        bl_json_obj_end(&j);
    }
    bl_json_obj_end(&j);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}

size_t sync_event_build(const char *source, uint64_t ts, const char *event_type, const char *entity,
                        const char *payload, SyncReason reason, bool rc_set, uint32_t rc, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kstr(&j, "source", source ? source : "app");
    bl_json_kint(&j, "ts", (int64_t)ts);
    bl_json_kstr(&j, "event_type", event_type);
    bl_json_kstr_or_null(&j, "entity", entity);
    bl_json_kstr_or_null(&j, "payload", payload);
    bl_json_kstr_or_null(&j, "reason", reason == SyncReason_None ? NULL : sync_reason_name(reason));
    bl_json_key(&j, "rc");
    if (rc_set) {
        char t[12];
        snprintf(t, sizeof(t), "0x%08X", (unsigned)rc);
        bl_json_str(&j, t);
    } else {
        bl_json_null(&j);
    }
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}
