// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_system.h"

#include <stdio.h>

#include "bl_json.h"
#include "sync_state.h"   // SYNC_SCHEMA

static void kversion(BlJson *j, const char *key, bool ok, unsigned a, unsigned b, unsigned c)
{
    bl_json_key(j, key);
    if (!ok) {
        bl_json_null(j);
        return;
    }
    char v[24];
    snprintf(v, sizeof(v), "%u.%u.%u", a, b, c);
    bl_json_str(j, v);
}

static void space(BlJson *j, const char *key, const ConsoleSpace *s)
{
    bl_json_key(j, key);
    bl_json_obj(j);
    bl_json_kint_or_null(j, "free_mb", s && s->free_mb >= 0, s ? s->free_mb : 0);
    bl_json_kint_or_null(j, "total_mb", s && s->total_mb > 0, s ? s->total_mb : 0);
    bl_json_obj_end(j);
}

size_t sync_system_build(const SyncSystem *s, char *out, size_t cap)
{
    // What is not published reads as unknown (sync.conf's publish_*; no
    // settings: everything).
    const SyncConf *c = s->conf;
    const bool hw = !c || sync_conf_publishes(c, SyncPublish_Hardware);
    const bool con = !c || sync_conf_publishes(c, SyncPublish_Console);
    const bool net = !c || sync_conf_publishes(c, SyncPublish_Network);
    const SysInfo *si = hw ? s->sys : NULL;
    const ConsoleLive *lh = hw ? s->live : NULL, *lc = con ? s->live : NULL, *ln = net ? s->live : NULL;
    const ConsoleSpace *sd = con ? s->sd : NULL, *nand = con ? s->nand : NULL;
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kstr(&j, "platform", SYNC_PLATFORM);   // what the console is, whatever is published
    bl_json_kint_or_null(&j, "ts", s->ts != 0, (int64_t)s->ts);

    bl_json_key(&j, "versions");
    bl_json_obj(&j);
    const uint32_t hos = si ? si->hos_version : 0;
    kversion(&j, "firmware", hos != 0, (hos >> 16) & 0xFF, (hos >> 8) & 0xFF, hos & 0xFF);
    kversion(&j, "atmosphere", si && si->ams_valid, si ? si->ams_major : 0, si ? si->ams_minor : 0,
             si ? si->ams_micro : 0);
    bl_json_kstr_or_null(&j, "hekate", hw ? s->hekate : NULL);
    bl_json_kstr_or_null(&j, "app", s->app_version);
    bl_json_kstr_or_null(&j, "agent", s->agent_version);
    bl_json_obj_end(&j);

    if (s->health) {
        const SyncAgentHealth *h = s->health;
        bl_json_key(&j, "agent");
        bl_json_obj(&j);
        bl_json_kint(&j, "uptime_s", h->uptime_s);
        bl_json_kint_or_null(&j, "heap_used", h->heap_used >= 0, h->heap_used);
        bl_json_kint_or_null(&j, "heap_total", h->heap_total > 0, h->heap_total);
        bl_json_key(&j, "protocols");
        bl_json_obj(&j);
        bl_json_kint(&j, "relay", h->relay_protocol);
        bl_json_kint(&j, "ipc", h->ipc_protocol);
        bl_json_obj_end(&j);
        bl_json_kbool(&j, "module", h->module);
        bl_json_obj_end(&j);
    }

    bl_json_key(&j, "hardware");
    bl_json_obj(&j);
    bl_json_kstr_or_null(&j, "model", lh ? console_model_name(lh->model) : NULL);
    bl_json_kstr_or_null(&j, "nickname", lh ? lh->nickname : NULL);
    bl_json_kstr_or_null(&j, "region", lh ? lh->region : NULL);
    const SysStorage st = si ? sysinfo_storage(si) : SysStorage_Unknown;
    bl_json_kstr_or_null(&j, "system", st == SysStorage_EmuMMC ? "emummc" : st == SysStorage_SysMMC ? "sysmmc" : NULL);
    bl_json_kbool_or_null(&j, "prodinfo_blanked", si && si->blank_valid, si && si->blank);
    bl_json_kstr_or_null(&j, "serial", hw ? s->serial : NULL);
    bl_json_obj_end(&j);

    bl_json_key(&j, "storage");
    bl_json_obj(&j);
    space(&j, "sd", sd);
    space(&j, "nand", nand);
    bl_json_obj_end(&j);

    const ConsoleLive *l = lc;
    bl_json_key(&j, "power");
    bl_json_obj(&j);
    bl_json_kint_or_null(&j, "battery_pct", l && l->battery_pct >= 0, l ? l->battery_pct : 0);
    // Charging: a charger that gives enough power or some (the official one, the dock, USB-PD).
    bl_json_kbool_or_null(&j, "charging", l && l->charger >= 0, l && (l->charger == 1 || l->charger == 2));
    bl_json_kstr_or_null(&j, "charger", l ? console_charger_name(l->charger) : NULL);
    bl_json_kbool_or_null(&j, "docked", l && l->docked >= 0, l && l->docked == 1);
    bl_json_kint_or_null(&j, "temperature_c", l && l->temperature_c > -1000, l ? l->temperature_c : 0);
    bl_json_obj_end(&j);

    bl_json_key(&j, "network");
    bl_json_obj(&j);
    bl_json_kstr_or_null(&j, "type", ln ? console_network_name(ln->network) : NULL);
    bl_json_kstr_or_null(&j, "ip", ln ? ln->ip : NULL);
    bl_json_kint_or_null(&j, "wifi_bars", ln && ln->wifi_bars >= 0, ln ? ln->wifi_bars : 0);
    bl_json_kstr_or_null(&j, "ssid", ln ? ln->ssid : NULL);
    bl_json_obj_end(&j);

    // When it started, to the minute: the same from one document to the next.
    const bool boot = l && l->uptime_s && s->ts > l->uptime_s;
    bl_json_kint_or_null(&j, "boot_ts", boot, boot ? (int64_t)((s->ts - l->uptime_s) / 60 * 60) : 0);

    bl_json_obj_end(&j);
    return bl_json_end(&j);
}
