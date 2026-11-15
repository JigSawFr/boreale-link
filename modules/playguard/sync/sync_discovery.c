// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_discovery.h"

#include <stdio.h>
#include <string.h>

#include "bl_json.h"
#include "sync_entities.h"

#define REPO_URL "https://github.com/JigSawFr/PlayGuard"

size_t sync_topic_base(const SyncConf *conf, char *out, size_t cap)
{
    char id[SYNC_ID_LEN + 1];
    sync_conf_device_id(conf, id);
    const int n = snprintf(out, cap, "%s/%s", conf->topic_prefix, id);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

bool sync_topic_is_device(const char *sub)
{
    return !strcmp(sub, "availability") || !strcmp(sub, "capabilities") || !strcmp(sub, "system") ||
           !strcmp(sub, "integration") || !strcmp(sub, "notices") || !strncmp(sub, "icon/", 5);
}

size_t sync_topic_sub(const char *sub, char *out, size_t cap)
{
    const int n = sync_topic_is_device(sub) ? snprintf(out, cap, "%s", sub)
                                             : snprintf(out, cap, SYNC_MODULE "/%s", sub);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

size_t sync_topic(const SyncConf *conf, const char *sub, char *out, size_t cap)
{
    char base[96], rel[96];
    if (!sync_topic_base(conf, base, sizeof(base)) || !sync_topic_sub(sub, rel, sizeof(rel))) return 0;
    const int n = snprintf(out, cap, "%s/%s", base, rel);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

size_t sync_discovery_topic(const SyncConf *conf, char *out, size_t cap)
{
    char id[SYNC_ID_LEN + 1];
    sync_conf_device_id(conf, id);
    const int n = snprintf(out, cap, "%s/device/boreale_%s/config", conf->discovery_prefix, id);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

void sync_device_of(const SysInfo *sys, const ConsoleLive *live, SyncDevice *out)
{
    memset(out, 0, sizeof(*out));
    if (live) {
        out->model = live->model;
        snprintf(out->nickname, sizeof(out->nickname), "%s", live->nickname);
    }
    if (!sys) return;
    out->hos_version = sys->hos_version;
    if (sys->ams_valid)
        out->ams_version = ((uint32_t)sys->ams_major << 16) | ((uint32_t)sys->ams_minor << 8) | sys->ams_micro;
}

// Nintendo's name and model number (on the back of the console).
static void model_of(ConsoleModel m, const char **name, const char **number)
{
    *number = NULL;
    switch (m) {
    case ConsoleModel_Erista: *name = "Nintendo Switch", *number = "HAC-001"; break;
    case ConsoleModel_Mariko: *name = "Nintendo Switch", *number = "HAC-001(-01)"; break;
    case ConsoleModel_Lite:   *name = "Nintendo Switch Lite", *number = "HDH-001"; break;
    case ConsoleModel_Oled:   *name = "Nintendo Switch \xe2\x80\x93 OLED Model", *number = "HEG-001"; break;
    default:                  *name = "Nintendo Switch"; break;
    }
}

// "23.0.1 · Atmosphère 1.10.1"; "" unknown.
static void system_of(const SyncDevice *dev, char *out, size_t cap)
{
    out[0] = '\0';
    if (!dev || !dev->hos_version) return;
    const uint32_t h = dev->hos_version, a = dev->ams_version;
    int n = snprintf(out, cap, "%u.%u.%u", (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
    if (a && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, " \xc2\xb7 Atmosph\xc3\xa8re %u.%u.%u", (a >> 16) & 0xFF, (a >> 8) & 0xFF,
                 a & 0xFF);
}

static bool announced(const SyncEntity *e, const SyncDiscovery *d)
{
    if (!sync_conf_publishes(d->conf, e->publish)) return false;   // what it shows is not published
    if (e->write == SyncWrite_None) return true;
    if (d->read_only) return false;
    if (e->write == SyncWrite_Timer && !d->conf->remote_timer_writes) return false;
    if (!strcmp(e->id, "profile") && d->n_profiles == 0) return false;   // nothing to choose from
    return true;
}

size_t sync_discovery_build(const SyncDiscovery *d, char *out, size_t cap)
{
    const SyncConf *c = d->conf;
    char topic[160], uid[96], key[64], sub[64], id[SYNC_ID_LEN + 1], name[SYNC_DEVICE_NAME_MAX], system[48];
    sync_conf_device_id(c, id);
    sync_conf_device_name(c, d->device ? d->device->nickname : NULL, name, sizeof(name));
    const char *version = d->sw_version && *d->sw_version ? d->sw_version : "unknown";

    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);

    bl_json_key(&j, "dev");
    bl_json_obj(&j);
    snprintf(uid, sizeof(uid), "boreale_%s", id);
    bl_json_key(&j, "ids");
    bl_json_arr(&j);
    bl_json_str(&j, uid);
    bl_json_arr_end(&j);
    bl_json_kstr(&j, "name", name);
    // The model and the system are versions and hardware: unknown while not published.
    const SyncDevice *dev = c->publish_hardware ? d->device : NULL;
    const char *model, *number;
    model_of(dev ? dev->model : ConsoleModel_Unknown, &model, &number);
    bl_json_kstr(&j, "mf", "Nintendo");
    bl_json_kstr(&j, "mdl", model);
    if (number) bl_json_kstr(&j, "mdl_id", number);
    system_of(dev, system, sizeof(system));
    if (system[0]) bl_json_kstr(&j, "sw", system);
    bl_json_obj_end(&j);

    bl_json_key(&j, "o");
    bl_json_obj(&j);
    bl_json_kstr(&j, "name", "PlayGuard");
    bl_json_kstr(&j, "sw", version);
    bl_json_kstr(&j, "url", REPO_URL);
    bl_json_obj_end(&j);

    sync_topic(c, "availability", topic, sizeof(topic));
    bl_json_kstr(&j, "avty_t", topic);

    bl_json_key(&j, "cmps");
    bl_json_obj(&j);
    size_t n = 0;
    const SyncEntity *list = sync_entities(&n);
    for (size_t i = 0; i < n; i++) {
        const SyncEntity *e = &list[i];
        // Each module's components under its name: a console with more
        // than one homebrew is still one device.
        snprintf(key, sizeof(key), SYNC_MODULE "_%s", e->id);
        bl_json_key(&j, key);
        bl_json_obj(&j);
        bl_json_kstr(&j, "p", e->platform);
        // Not offered now (remote_timer_writes off, read-only, no profile):
        // its platform alone, which removes it from Home Assistant if an
        // earlier discovery had it, rather than leaving it there to refuse
        // every order.
        if (!announced(e, d)) {
            bl_json_obj_end(&j);
            continue;
        }
        bl_json_kstr(&j, "name", e->name);
        snprintf(uid, sizeof(uid), "boreale_%s_" SYNC_MODULE "_%s", id, e->id);
        bl_json_kstr(&j, "uniq_id", uid);
        if (!strcmp(e->platform, "event")) {
            sync_topic(c, "event", topic, sizeof(topic));
            bl_json_kstr(&j, "stat_t", topic);
            bl_json_key(&j, "evt_typ");
            bl_json_arr(&j);
            // The events the engine sends: the orders' (sync_engine.c's
            // event_type()), and what the console did on its own (sync_watch.c).
            static const char *const types[] = { "command_applied", "command_rejected", "command_waiting",
                                                 "play_started",    "play_stopped",     "time_warning",
                                                 "play_blocked" };
            for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) bl_json_str(&j, types[t]);
            bl_json_arr_end(&j);
        } else if (e->value_template) {
            sync_topic(c, e->topic ? e->topic : "state", topic, sizeof(topic));
            bl_json_kstr(&j, "stat_t", topic);
            bl_json_kstr(&j, "val_tpl", e->value_template);
        }
        if (e->write != SyncWrite_None) {
            snprintf(sub, sizeof(sub), "%s/set", e->id);
            sync_topic(c, sub, topic, sizeof(topic));
            bl_json_kstr(&j, "cmd_t", topic);
            // Retained: an order sent while the console sleeps waits on the
            // broker. Optimistic: the entity keeps the value asked meanwhile.
            bl_json_kbool(&j, "ret", true);
            if (e->value_template) bl_json_kbool(&j, "opt", true);
        }
        if (!strcmp(e->id, "profile")) {
            bl_json_key(&j, "ops");
            bl_json_arr(&j);
            for (size_t p = 0; p < d->n_profiles; p++) bl_json_str(&j, d->profiles[p]);
            bl_json_arr_end(&j);
        }
        // Attributes from the same document as the state.
        if (e->extra && strstr(e->extra, "\"json_attr_tpl\"")) {
            sync_topic(c, e->topic ? e->topic : "state", topic, sizeof(topic));
            bl_json_kstr(&j, "json_attr_t", topic);
        }
        if (e->extra && *e->extra) bl_json_members(&j, e->extra);
        bl_json_obj_end(&j);
    }
    const SyncRetired *gone = sync_entities_retired(&n);
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof(key), SYNC_MODULE "_%s", gone[i].id);
        bl_json_key(&j, key);
        bl_json_obj(&j);
        bl_json_kstr(&j, "p", gone[i].platform);
        bl_json_obj_end(&j);
    }
    bl_json_obj_end(&j);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}
