// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_conf.h"

#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

void sync_conf_defaults(SyncConf *c)
{
    memset(c, 0, sizeof(*c));
    bl_kv_file_init(&c->file, c->kept, sizeof(c->kept));
    c->file.schema = SYNC_CONF_SCHEMA;
    c->port = 1883;
    c->policy = SyncPolicy_Ask;
    c->publish_activity = c->publish_games = c->publish_icons = c->publish_console = c->publish_network = c->publish_hardware = true;
    c->ha_discovery = true;
    c->poll_s = 30;
    copy(c->topic_prefix, sizeof(c->topic_prefix), "boreale");
    copy(c->discovery_prefix, sizeof(c->discovery_prefix), "homeassistant");
    c->log_level = 1;
}

bool sync_conf_id_valid(const char *id)
{
    if (!id || strlen(id) != SYNC_ID_LEN) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

void sync_conf_device_id(const SyncConf *c, char out[SYNC_ID_LEN + 1])
{
    out[0] = '\0';
    if (!sync_conf_id_valid(c->console_id)) return;
    memcpy(out, c->console_id, SYNC_ID_LEN + 1);
    if (!c->emummc) return;
    const char d = out[0];
    const int v = (d >= '0' && d <= '9' ? d - '0' : d - 'a' + 10) ^ 8;
    out[0] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
}

void sync_conf_device_name(const SyncConf *c, const char *nickname, char *out, size_t cap)
{
    if (!cap) return;
    const char *name = c->console_name[0]                                 ? c->console_name
                       : c->publish_hardware && nickname && nickname[0] ? nickname
                                                                          : "Nintendo Switch";
    snprintf(out, cap, "%s%s", name, c->emummc ? " (emuMMC)" : "");
}

// A topic level we put our own names under: letters, digits, '_' and '-'.
static bool prefix_valid(const char *s)
{
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        const char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

const char *sync_mqtt_version_name(SyncMqttVersion v)
{
    switch (v) {
        case SyncMqtt_V5: return "5";
        case SyncMqtt_V311: return "3.1.1";
        default: return "auto";
    }
}

const char *sync_policy_name(SyncPolicy p)
{
    switch (p) {
        case SyncPolicy_Auto: return "auto";
        case SyncPolicy_Off: return "off";
        default: return "ask";
    }
}

typedef struct {
    SyncConf *c;
    int read;
    bool prefix_read;   // topic_prefix was in the file
} ParseCtx;

// Whether the file has a `schema` line: bl_kv_read() reads none as schema 1,
// but this file's rule is that none is the current one (a file written by hand
// without it is not an old one, and must not be moved to the new prefix).
static void on_schema_line(void *ctx, const char *key, const char *value)
{
    (void)value;
    if (!strcmp(key, "schema")) *(bool *)ctx = true;
}

static void str_field(char *dst, size_t cap, const char *value)
{
    if (strlen(value) < cap) copy(dst, cap, value);
}

static void on_kv(void *vctx, const char *key, const char *value)
{
    ParseCtx *p = (ParseCtx *)vctx;
    SyncConf *c = p->c;
    uint32_t u = 0;
    bool b = false;
    p->read++;
    if (!strcmp(key, "enabled")) {
        c->broker_keys = true;
        if (bl_parse_bool(value, &b)) c->enabled = b;
    } else if (!strcmp(key, "host")) {
        c->broker_keys = true;
        str_field(c->host, sizeof(c->host), value);
    } else if (!strcmp(key, "port")) {
        c->broker_keys = true;
        if (bl_parse_uint(value, 1, 65535, &u)) c->port = (uint16_t)u;
    } else if (!strcmp(key, "tls")) {
        c->broker_keys = true;
        if (bl_parse_bool(value, &b)) c->tls = b;
    } else if (!strcmp(key, "ca_file")) {
        c->broker_keys = true;
        str_field(c->ca_file, sizeof(c->ca_file), value);
    } else if (!strcmp(key, "mqtt_version")) {
        c->broker_keys = true;
        if (!strcmp(value, "auto")) c->mqtt_version = SyncMqtt_Auto;
        else if (!strcmp(value, "5") || !strcmp(value, "5.0")) c->mqtt_version = SyncMqtt_V5;
        else if (!strcmp(value, "3.1.1") || !strcmp(value, "4")) c->mqtt_version = SyncMqtt_V311;
    } else if (!strcmp(key, "username")) {
        c->broker_keys = true;
        str_field(c->username, sizeof(c->username), value);
    } else if (!strcmp(key, "password")) {
        c->broker_keys = true;
        str_field(c->password, sizeof(c->password), value);
    } else if (!strcmp(key, "allow_anonymous")) {
        c->broker_keys = true;
        if (bl_parse_bool(value, &b)) c->allow_anonymous = b;
    } else if (!strcmp(key, "console_id")) {
        c->broker_keys = true;
        if (sync_conf_id_valid(value)) copy(c->console_id, sizeof(c->console_id), value);
    } else if (!strcmp(key, "console_name")) {
        c->broker_keys = true;
        str_field(c->console_name, sizeof(c->console_name), value);
    } else if (!strcmp(key, "policy")) {
        if (!strcmp(value, "ask")) c->policy = SyncPolicy_Ask;
        else if (!strcmp(value, "auto")) c->policy = SyncPolicy_Auto;
        else if (!strcmp(value, "off")) c->policy = SyncPolicy_Off;
    } else if (!strcmp(key, "remote_timer_writes")) {
        if (bl_parse_bool(value, &b)) c->remote_timer_writes = b;
    } else if (!strcmp(key, "publish_report")) {
        if (bl_parse_bool(value, &b)) c->publish_report = b;
    } else if (!strcmp(key, "publish_activity")) {
        if (bl_parse_bool(value, &b)) c->publish_activity = b;
    } else if (!strcmp(key, "publish_games")) {
        if (bl_parse_bool(value, &b)) c->publish_games = b;
    } else if (!strcmp(key, "publish_icons")) {
        if (bl_parse_bool(value, &b)) c->publish_icons = b;
    } else if (!strcmp(key, "publish_console")) {
        if (bl_parse_bool(value, &b)) c->publish_console = b;
    } else if (!strcmp(key, "publish_network")) {
        if (bl_parse_bool(value, &b)) c->publish_network = b;
    } else if (!strcmp(key, "publish_hardware")) {
        if (bl_parse_bool(value, &b)) c->publish_hardware = b;
    } else if (!strcmp(key, "publish_serial")) {
        if (bl_parse_bool(value, &b)) c->publish_serial = b;
    } else if (!strcmp(key, "ha_discovery")) {
        if (bl_parse_bool(value, &b)) c->ha_discovery = b;
    } else if (!strcmp(key, "poll_s")) {
        if (bl_parse_uint(value, 10, 300, &u)) c->poll_s = (uint16_t)u;
    } else if (!strcmp(key, "topic_prefix")) {
        c->broker_keys = true;
        if (strlen(value) < sizeof(c->topic_prefix) && prefix_valid(value)) {
            copy(c->topic_prefix, sizeof(c->topic_prefix), value);
            p->prefix_read = true;
        }
    } else if (!strcmp(key, "legacy_prefix")) {
        if (strlen(value) < sizeof(c->legacy_prefix) && prefix_valid(value))
            copy(c->legacy_prefix, sizeof(c->legacy_prefix), value);
    } else if (!strcmp(key, "discovery_prefix")) {
        c->broker_keys = true;
        if (strlen(value) < sizeof(c->discovery_prefix) && prefix_valid(value))
            copy(c->discovery_prefix, sizeof(c->discovery_prefix), value);
    } else if (!strcmp(key, "log_level")) {
        if (bl_parse_uint(value, 0, 2, &u)) c->log_level = (int)u;
    } else {
        bl_kv_keep(&c->file, key, value);
    }
}

bool sync_conf_parse(SyncConf *c, const char *text, size_t len)
{
    ParseCtx p = { c, 0, false };
    bl_kv_file_init(&c->file, c->kept, sizeof(c->kept));   // its own buffer, even in a copy
    c->broker_keys = false;
    bool has_schema = false;
    bl_kv_each(text, len, on_schema_line, &has_schema);
    bl_kv_read(&c->file, text, len, on_kv, &p);
    if (!has_schema) c->file.schema = SYNC_CONF_SCHEMA;
    else p.read++;   // the schema line is a line read, though bl_kv_read() keeps it from on_kv
    // Schema 1 published under <prefix>/<id>/ with "playguard" by default:
    // the default moves to "boreale", and the old tree is cleared once.
    if (p.read > 0 && c->file.schema == 1) {
        const char *old = p.prefix_read ? c->topic_prefix : "playguard";
        if (!c->legacy_prefix[0]) copy(c->legacy_prefix, sizeof(c->legacy_prefix), old);
        if (!strcmp(old, "playguard")) copy(c->topic_prefix, sizeof(c->topic_prefix), "boreale");
    }
    return p.read > 0 || len == 0;
}

size_t sync_conf_write(const SyncConf *c, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
        "# PlayGuard's remote link settings (MQTT / Home Assistant), the broker's\n"
        "# being in config/boreale/link.conf. Written by PlayGuard's Remote access\n"
        "# screen; see docs/sync-protocol.md.\n"
        "schema=%d\n"
        "policy=%s\n"
        "remote_timer_writes=%d\n"
        "publish_report=%d\n"
        "publish_activity=%d\n"
        "publish_games=%d\n"
        "publish_icons=%d\n"
        "publish_console=%d\n"
        "publish_network=%d\n"
        "publish_hardware=%d\n"
        "publish_serial=%d\n"
        "ha_discovery=%d\n"
        "poll_s=%u\n"
        "log_level=%d\n"
        "%s%s%s",
        SYNC_CONF_SCHEMA, sync_policy_name(c->policy), c->remote_timer_writes,
        c->publish_report, c->publish_activity, c->publish_games, c->publish_icons, c->publish_console, c->publish_network,
        c->publish_hardware, c->publish_serial, c->ha_discovery, (unsigned)c->poll_s, c->log_level,
        c->legacy_prefix[0] ? "legacy_prefix=" : "", c->legacy_prefix, c->legacy_prefix[0] ? "\n" : "");
    if (n < 0 || (size_t)n >= cap) return 0;
    BlKvFile kept = c->file;
    kept.kept = (char *)c->kept;   // this SyncConf's lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, (size_t)n);
}

const char *sync_conf_unwritable(const SyncConf *c)
{
    if (c->file.schema > SYNC_CONF_SCHEMA || c->file.schema == 0) return "written by a newer version";
    if (!bl_kv_may_rewrite(&c->file, SYNC_CONF_SCHEMA)) return "a line that cannot be read or kept";
    return NULL;
}

bool sync_conf_publishes(const SyncConf *c, SyncPublish p)
{
    switch (p) {
    case SyncPublish_Activity: return c->publish_activity;
    case SyncPublish_Games:    return c->publish_games;
    case SyncPublish_Icons:    return c->publish_icons;
    case SyncPublish_Console:  return c->publish_console;
    case SyncPublish_Network:  return c->publish_network;
    case SyncPublish_Hardware: return c->publish_hardware;
    case SyncPublish_Serial:   return c->publish_hardware && c->publish_serial;
    case SyncPublish_Report:   return c->publish_report;
    default:                   return true;
    }
}

const char *sync_publish_name(SyncPublish p)
{
    static const char *const NAMES[SyncPublish_Count] = { "controls", "activity", "games",  "icons",  "console",
                                                          "network",  "hardware", "serial", "report" };
    return p < SyncPublish_Count ? NAMES[p] : "controls";
}

const char *sync_conf_problem(const SyncConf *c)
{
    if (!c->host[0]) return "no broker host";
    if (!sync_conf_id_valid(c->console_id)) return "no console id";
    if (!c->username[0] && !c->allow_anonymous) return "no user name (anonymous brokers must be allowed explicitly)";
    // Line breaks would end up in the file: the next read would split them.
    const char *fields[] = { c->host, c->username, c->password, c->console_name, c->ca_file };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (strchr(fields[i], '\n') || strchr(fields[i], '\r')) return "a setting holds a line break";
    return NULL;
}

_Static_assert(sizeof(((SyncConf *)0)->host) == BL_LINK_HOST_MAX && sizeof(((SyncConf *)0)->password) == BL_LINK_PASS_MAX &&
                   sizeof(((SyncConf *)0)->username) == BL_LINK_USER_MAX && sizeof(((SyncConf *)0)->ca_file) == BL_LINK_PATH_MAX &&
                   sizeof(((SyncConf *)0)->console_name) == BL_LINK_NAME_MAX &&
                   sizeof(((SyncConf *)0)->topic_prefix) == BL_LINK_PREFIX_MAX,
               "sync.conf's link fields are link.conf's");
_Static_assert((int)SyncMqtt_V5 == (int)BlLinkMqtt_V5 && (int)SyncMqtt_V311 == (int)BlLinkMqtt_V311, "the MQTT versions");

void sync_conf_to_link(const SyncConf *c, BlLinkConf *l)
{
    l->enabled = c->enabled;
    copy(l->host, sizeof(l->host), c->host);
    l->port = c->port;
    l->tls = c->tls;
    copy(l->ca_file, sizeof(l->ca_file), c->ca_file);
    l->mqtt_version = (BlLinkMqtt)c->mqtt_version;
    copy(l->username, sizeof(l->username), c->username);
    copy(l->password, sizeof(l->password), c->password);
    l->allow_anonymous = c->allow_anonymous;
    copy(l->console_id, sizeof(l->console_id), c->console_id);
    copy(l->console_name, sizeof(l->console_name), c->console_name);
    copy(l->topic_prefix, sizeof(l->topic_prefix), c->topic_prefix);
    copy(l->discovery_prefix, sizeof(l->discovery_prefix), c->discovery_prefix);
}

void sync_conf_from_link(SyncConf *c, const BlLinkConf *l)
{
    c->enabled = l->enabled;
    copy(c->host, sizeof(c->host), l->host);
    c->port = l->port;
    c->tls = l->tls;
    copy(c->ca_file, sizeof(c->ca_file), l->ca_file);
    c->mqtt_version = (SyncMqttVersion)l->mqtt_version;
    copy(c->username, sizeof(c->username), l->username);
    copy(c->password, sizeof(c->password), l->password);
    c->allow_anonymous = l->allow_anonymous;
    copy(c->console_id, sizeof(c->console_id), l->console_id);
    copy(c->console_name, sizeof(c->console_name), l->console_name);
    copy(c->topic_prefix, sizeof(c->topic_prefix), l->topic_prefix);
    copy(c->discovery_prefix, sizeof(c->discovery_prefix), l->discovery_prefix);
}
