// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_entities.h"

#include <string.h>

// Discovery abbreviations (HA): unit_of_meas, dev_cla, stat_cla, ent_cat,
// pl_on / pl_off (binary_sensor, switch commands), stat_on / stat_off
// (switch state), ops (select options), cmd_tpl (command template), en
// (enabled by default), ic (icon), sug_dsp_prc (decimals shown).
#define E_MINUTES "\"unit_of_meas\":\"min\",\"dev_cla\":\"duration\""
#define E_MEASURE E_MINUTES ",\"stat_cla\":\"measurement\""
// A daily total, back to 0 at midnight (HA's "total_increasing", like a daily meter).
#define E_TOTAL E_MINUTES ",\"stat_cla\":\"total_increasing\""
#define E_BINARY "\"pl_on\":\"True\",\"pl_off\":\"False\""
#define E_SWITCH "\"stat_on\":\"True\",\"stat_off\":\"False\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\""
#define E_LIMIT "\"min\":0,\"max\":1440,\"step\":5,\"mode\":\"box\"," E_MINUTES ",\"ent_cat\":\"config\""
#define E_DIAG "\"ent_cat\":\"diagnostic\""
#define E_CONFIG "\"ent_cat\":\"config\""
// Off until turned on on the device page: what few dashboards need.
#define E_OFF ",\"en\":false"
#define E_SPACE "\"unit_of_meas\":\"MiB\",\"dev_cla\":\"data_size\",\"stat_cla\":\"measurement\""
// The document an entity's template reads (NULL: the state) and what it
// shows (sync.conf's publish_* switches).
#define D_STATE NULL, SyncPublish_Controls
#define D_ACTIVITY NULL, SyncPublish_Activity   // the state's activity_today
#define D_VERSIONS NULL, SyncPublish_Hardware   // the state's firmware and Atmosphère
#define T_CONSOLE "system", SyncPublish_Console
#define T_NETWORK "system", SyncPublish_Network
#define T_HARDWARE "system", SyncPublish_Hardware
#define T_SERIAL "system", SyncPublish_Serial
#define T_GAMES "games", SyncPublish_Games

static const SyncEntity ENTITIES[] = {
    // binary sensors
    { "parental_controls_enabled", "binary_sensor", SyncWrite_None, "Parental controls set up",
      "{{ value_json.controls.enabled }}", E_BINARY ",\"ic\":\"mdi:shield-account\"", D_STATE },
    { "pin_set", "binary_sensor", SyncWrite_None, "PIN set", "{{ value_json.controls.pin_set }}",
      E_BINARY ",\"ic\":\"mdi:form-textbox-password\"," E_DIAG, D_STATE },
    { "companion_linked", "binary_sensor", SyncWrite_None, "Companion app linked",
      "{{ value_json.controls.companion_linked }}", E_BINARY ",\"ic\":\"mdi:cellphone-link\"", D_STATE },
    { "timer_enabled", "binary_sensor", SyncWrite_None, "Play timer", "{{ value_json.timer.enabled }}",
      E_BINARY ",\"ic\":\"mdi:timer-outline\"", D_STATE },
    // Why: its `reason` attribute (the bedtime alarm's hours, or the day's limit).
    { "limit_reached", "binary_sensor", SyncWrite_None, "Playing blocked", "{{ value_json.timer.limit_reached }}",
      E_BINARY ",\"ic\":\"mdi:block-helper\",\"json_attr_tpl\":\"{{ {'reason': value_json.timer.blocked_by} | tojson }}\"",
      D_STATE },
    { "clock_accurate", "binary_sensor", SyncWrite_None, "Network clock accurate",
      "{{ value_json.clock_accurate }}", E_BINARY ",\"ic\":\"mdi:clock-check-outline\"," E_DIAG E_OFF, D_STATE },

    // sensors
    // Today's play time is the play log's (every game, whoever played it);
    // the play timer's own count, which only counts while a limit is set, is off.
    { "used_screen_time", "sensor", SyncWrite_None, "Used screen time", "{{ value_json.timer.used_min }}", E_TOTAL E_OFF,
      D_STATE },
    { "used_screen_time_log", "sensor", SyncWrite_None, "Play time today", "{{ value_json.activity_today.used_min }}",
      E_TOTAL, D_ACTIVITY },
    { "screen_time_remaining", "sensor", SyncWrite_None, "Screen time remaining",
      "{{ value_json.timer.remaining_min }}", E_MEASURE, D_STATE },
    { "extended_screen_time", "sensor", SyncWrite_None, "Extended screen time",
      "{{ value_json.timer.extended_today_min }}", E_MINUTES, D_STATE },
    { "now_playing", "sensor", SyncWrite_None, "Now playing",
      "{{ value_json.activity_today.now_playing.name or value_json.activity_today.now_playing.app_id }}",
      "\"ic\":\"mdi:gamepad-variant\"", D_ACTIVITY },
    { "rating_age", "sensor", SyncWrite_None, "Age rating", "{{ value_json.controls.rating_age }}", E_DIAG E_OFF, D_STATE },
    // Off: the device's own versions already show both (sw_version).
    { "firmware", "sensor", SyncWrite_None, "Firmware", "{{ value_json.console.firmware }}", E_DIAG E_OFF, D_VERSIONS },
    { "atmosphere", "sensor", SyncWrite_None, "Atmosphère", "{{ value_json.console.atmosphere }}", E_DIAG E_OFF, D_VERSIONS },
    { "spent_raw", "sensor", SyncWrite_None, "Time spent (1952)", "{{ value_json.timer.spent_raw_min }}",
      E_MINUTES "," E_DIAG E_OFF, D_STATE },
    { "last_order", "sensor", SyncWrite_None, "Last order", "{{ value_json.link.last_result }}", E_DIAG E_OFF, D_STATE },

    // limits: the day's and every day's on, each day's off until wanted
    { "limit_sun", "number", SyncWrite_Timer, "Limit Sunday", "{{ value_json.timer.limits.sun }}", E_LIMIT E_OFF, D_STATE },
    { "limit_mon", "number", SyncWrite_Timer, "Limit Monday", "{{ value_json.timer.limits.mon }}", E_LIMIT E_OFF, D_STATE },
    { "limit_tue", "number", SyncWrite_Timer, "Limit Tuesday", "{{ value_json.timer.limits.tue }}", E_LIMIT E_OFF, D_STATE },
    { "limit_wed", "number", SyncWrite_Timer, "Limit Wednesday", "{{ value_json.timer.limits.wed }}", E_LIMIT E_OFF, D_STATE },
    { "limit_thu", "number", SyncWrite_Timer, "Limit Thursday", "{{ value_json.timer.limits.thu }}", E_LIMIT E_OFF, D_STATE },
    { "limit_fri", "number", SyncWrite_Timer, "Limit Friday", "{{ value_json.timer.limits.fri }}", E_LIMIT E_OFF, D_STATE },
    { "limit_sat", "number", SyncWrite_Timer, "Limit Saturday", "{{ value_json.timer.limits.sat }}", E_LIMIT E_OFF, D_STATE },
    { "limit_uniform", "number", SyncWrite_Timer, "Limit every day", "{{ value_json.timer.uniform_min }}", E_LIMIT, D_STATE },
    { "max_screentime_today", "number", SyncWrite_Timer, "Max screentime today",
      "{{ value_json.timer.limit_today_min }}", E_LIMIT, D_STATE },

    // bedtime
    { "bedtime_alarm", "time", SyncWrite_Timer, "Bedtime", "{{ value_json.timer.bedtime.start }}",
      "\"ic\":\"mdi:weather-night\"," E_CONFIG, D_STATE },
    { "bedtime_end_time", "time", SyncWrite_Timer, "Wake-up time", "{{ value_json.timer.bedtime.end }}",
      "\"ic\":\"mdi:weather-sunset-up\"," E_CONFIG, D_STATE },
    { "bedtime_enabled", "switch", SyncWrite_Timer, "Bedtime alarm", "{{ value_json.timer.bedtime.enabled }}",
      E_SWITCH ",\"ic\":\"mdi:bed-clock\"," E_CONFIG, D_STATE },

    // switches
    { "console_lock", "switch", SyncWrite_Timer, "Block all games", "{{ value_json.timer.console_locked }}",
      E_SWITCH ",\"ic\":\"mdi:lock\"", D_STATE },
    { "play_timer_alarm", "switch", SyncWrite_Timer, "Time's up alarm", "{{ value_json.timer.alarm_on }}",
      E_SWITCH ",\"ic\":\"mdi:alarm\"," E_CONFIG, D_STATE },
    { "vr_restricted", "switch", SyncWrite_Free, "VR mode restricted", "{{ value_json.controls.vr_restricted }}",
      E_SWITCH ",\"ic\":\"mdi:virtual-reality\"," E_CONFIG, D_STATE },
    { "sns_post_restricted", "switch", SyncWrite_Free, "Posting to social media restricted",
      "{{ value_json.controls.sns_post_restricted }}", E_SWITCH ",\"ic\":\"mdi:share-off\"," E_CONFIG, D_STATE },
    { "free_communication_restricted", "switch", SyncWrite_Free, "Communication restricted",
      "{{ value_json.controls.free_communication_restricted }}", E_SWITCH ",\"ic\":\"mdi:message-lock\"," E_CONFIG,
      D_STATE },

    // The lock: locked while parental controls are in effect. Unlocking
    // (temporarily, with the stored PIN) needs remote_timer_writes; locking
    // again is always allowed, as on the console.
    { "parental_controls", "lock", SyncWrite_Free, "Parental controls",
      "{{ 'LOCKED' if value_json.controls.enabled and not value_json.controls.temp_unlocked else 'UNLOCKED' }}", "",
      D_STATE },

    // selects (the profile's options are added when the discovery is built).
    // The level "none" is shown as "unrestricted": HA reads a select's "None"
    // as unknown. The order still carries "none" (cmd_tpl).
    { "restriction_level", "select", SyncWrite_Free, "Restriction level",
      "{{ 'unrestricted' if value_json.controls.level == 'none' else value_json.controls.level }}",
      "\"ops\":[\"unrestricted\",\"young_child\",\"child\",\"teen\",\"custom\"],"
      "\"cmd_tpl\":\"{{ 'none' if value == 'unrestricted' else value }}\",\"ic\":\"mdi:account-child-circle\","
      E_CONFIG, D_STATE },
    // The profile in effect (the saved one whose limits are the timer's), or
    // none: "None", which Home Assistant's select reads as unknown.
    { "profile", "select", SyncWrite_Timer, "Profile", "{{ value_json.timer.profile }}",
      "\"ic\":\"mdi:account-switch\"," E_CONFIG, D_STATE },

    // buttons
    { "sync_now", "button", SyncWrite_Free, "Sync now", NULL, "\"ic\":\"mdi:sync\"," E_DIAG, D_STATE },
    { "export_report", "button", SyncWrite_Free, "Export a diagnostic report", NULL,
      "\"ic\":\"mdi:file-document-outline\"," E_DIAG E_OFF, D_STATE },
    { "add_bonus_time_15", "button", SyncWrite_Timer, "Extra time +15 min", NULL, "\"ic\":\"mdi:timer-plus-outline\"", D_STATE },
    { "add_bonus_time_30", "button", SyncWrite_Timer, "Extra time +30 min", NULL, "\"ic\":\"mdi:timer-plus-outline\"", D_STATE },
    { "add_bonus_time_60", "button", SyncWrite_Timer, "Extra time +1 h", NULL, "\"ic\":\"mdi:timer-plus-outline\"", D_STATE },
    { "stop_today", "button", SyncWrite_Timer, "No more play today", NULL, "\"ic\":\"mdi:timer-off-outline\"", D_STATE },
    { "remove_limit", "button", SyncWrite_Timer, "Remove the limit", NULL, "\"ic\":\"mdi:timer-remove-outline\"", D_STATE },

    // events (state topic and event types set when the discovery is built)
    { "events", "event", SyncWrite_None, "Events", NULL, "", D_STATE },

    // The console itself (the system document): versions, hardware, storage,
    // power, network. Diagnostic but for what a parent may want on a card.
    { "model", "sensor", SyncWrite_None, "Model", "{{ value_json.hardware.model }}",
      "\"dev_cla\":\"enum\",\"ops\":[\"erista\",\"mariko\",\"lite\",\"oled\",\"other\"]," E_DIAG E_OFF, T_HARDWARE },
    { "nickname", "sensor", SyncWrite_None, "Console nickname", "{{ value_json.hardware.nickname }}",
      "\"ic\":\"mdi:tag-text-outline\"," E_DIAG E_OFF, T_HARDWARE },
    { "region", "sensor", SyncWrite_None, "Region", "{{ value_json.hardware.region }}",
      "\"dev_cla\":\"enum\",\"ops\":[\"jpn\",\"usa\",\"eur\",\"aus\",\"htk\",\"chn\"]," E_DIAG E_OFF, T_HARDWARE },
    { "hekate", "sensor", SyncWrite_None, "hekate", "{{ value_json.versions.hekate }}", E_DIAG E_OFF, T_HARDWARE },
    { "system_storage", "sensor", SyncWrite_None, "System", "{{ value_json.hardware.system }}",
      "\"dev_cla\":\"enum\",\"ops\":[\"sysmmc\",\"emummc\"]," E_DIAG E_OFF, T_HARDWARE },
    { "prodinfo_blanked", "binary_sensor", SyncWrite_None, "PRODINFO blanked",
      "{{ value_json.hardware.prodinfo_blanked }}", E_BINARY "," E_DIAG E_OFF, T_HARDWARE },
    // Only with "Publish the serial number" on the console; off in Home Assistant too.
    { "serial", "sensor", SyncWrite_None, "Serial number", "{{ value_json.hardware.serial }}",
      E_DIAG E_OFF, T_SERIAL },
    { "sd_free", "sensor", SyncWrite_None, "SD card free space", "{{ value_json.storage.sd.free_mb }}",
      E_SPACE ",\"ic\":\"mdi:micro-sd\"", T_CONSOLE },
    { "sd_total", "sensor", SyncWrite_None, "SD card size", "{{ value_json.storage.sd.total_mb }}",
      E_SPACE ",\"ic\":\"mdi:micro-sd\"," E_DIAG E_OFF, T_CONSOLE },
    { "nand_free", "sensor", SyncWrite_None, "Internal storage free space", "{{ value_json.storage.nand.free_mb }}",
      E_SPACE ",\"ic\":\"mdi:harddisk\"", T_CONSOLE },
    { "nand_total", "sensor", SyncWrite_None, "Internal storage size", "{{ value_json.storage.nand.total_mb }}",
      E_SPACE ",\"ic\":\"mdi:harddisk\"," E_DIAG E_OFF, T_CONSOLE },
    { "battery", "sensor", SyncWrite_None, "Battery", "{{ value_json.power.battery_pct }}",
      "\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\",\"stat_cla\":\"measurement\"", T_CONSOLE },
    { "charging", "binary_sensor", SyncWrite_None, "Charging", "{{ value_json.power.charging }}",
      E_BINARY ",\"dev_cla\":\"battery_charging\"", T_CONSOLE },
    { "docked", "binary_sensor", SyncWrite_None, "Docked", "{{ value_json.power.docked }}",
      E_BINARY ",\"ic\":\"mdi:television\"", T_CONSOLE },
    { "temperature", "sensor", SyncWrite_None, "Temperature", "{{ value_json.power.temperature_c }}",
      "\"unit_of_meas\":\"\xc2\xb0" "C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",\"sug_dsp_prc\":0,"
      E_DIAG E_OFF, T_CONSOLE },
    { "network", "sensor", SyncWrite_None, "Connection", "{{ value_json.network.type }}",
      "\"dev_cla\":\"enum\",\"ops\":[\"none\",\"wifi\",\"ethernet\"]," E_DIAG E_OFF, T_NETWORK },
    { "ip_address", "sensor", SyncWrite_None, "IP address", "{{ value_json.network.ip }}",
      "\"ic\":\"mdi:ip-network\"," E_DIAG E_OFF, T_NETWORK },
    { "wifi_signal", "sensor", SyncWrite_None, "Wi-Fi signal", "{{ value_json.network.wifi_bars }}",
      "\"stat_cla\":\"measurement\",\"ic\":\"mdi:wifi\"," E_DIAG E_OFF, T_NETWORK },
    // The network's name: off in Home Assistant until wanted.
    { "ssid", "sensor", SyncWrite_None, "Wi-Fi network", "{{ value_json.network.ssid }}",
      "\"ic\":\"mdi:wifi\"," E_DIAG E_OFF, T_NETWORK },
    { "last_boot", "sensor", SyncWrite_None, "Last started",
      "{{ as_datetime(value_json.boot_ts) if value_json.boot_ts else None }}", "\"dev_cla\":\"timestamp\"," E_DIAG E_OFF,
      T_CONSOLE },

    // The games (the games document, PlayGuard's).
    { "games_installed", "sensor", SyncWrite_None, "Games installed", "{{ value_json.installed }}",
      "\"stat_cla\":\"measurement\",\"ic\":\"mdi:gamepad-square\"", T_GAMES },
    { "play_time_total", "sensor", SyncWrite_None, "Play time (all time)", "{{ value_json.total_min }}",
      E_MINUTES ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:timer-sand-complete\"", T_GAMES },
    { "accounts", "sensor", SyncWrite_None, "User accounts", "{{ value_json.accounts }}",
      "\"ic\":\"mdi:account-multiple\"," E_DIAG E_OFF, T_GAMES },
};

const SyncEntity *sync_entities(size_t *count)
{
    *count = sizeof(ENTITIES) / sizeof(ENTITIES[0]);
    return ENTITIES;
}

// Replaced by the parental controls lock. Their orders still parse,
// for an integration that sends them.
static const SyncRetired RETIRED[] = {
    { "temp_unlocked", "binary_sensor" },
    { "unlocked", "switch" },
    { "lock_now", "button" },
};

const SyncRetired *sync_entities_retired(size_t *count)
{
    *count = sizeof(RETIRED) / sizeof(RETIRED[0]);
    return RETIRED;
}

const SyncEntity *sync_entity_find(const char *id)
{
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++)
        if (!strcmp(ENTITIES[i].id, id)) return &ENTITIES[i];
    return NULL;
}
