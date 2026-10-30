// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_link_conf.h"

#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

void bl_link_conf_defaults(BlLinkConf *c)
{
    memset(c, 0, sizeof(*c));
    c->port = 1883;
    copy(c->topic_prefix, sizeof(c->topic_prefix), "boreale");
    copy(c->discovery_prefix, sizeof(c->discovery_prefix), "homeassistant");
    bl_kv_file_init(&c->file, c->kept, sizeof(c->kept));
    c->file.schema = BL_LINK_CONF_SCHEMA;
}

bool bl_link_id_valid(const char *id)
{
    if (!id || strlen(id) != BL_LINK_ID_LEN) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

bool bl_link_prefix_valid(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        const char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

const char *bl_link_mqtt_name(BlLinkMqtt v)
{
    switch (v) {
        case BlLinkMqtt_V5: return "5";
        case BlLinkMqtt_V311: return "3.1.1";
        default: return "auto";
    }
}

static void str_field(char *dst, size_t cap, const char *value)
{
    if (strlen(value) < cap) copy(dst, cap, value);
}

static void prefix_field(char *dst, size_t cap, const char *value)
{
    if (strlen(value) < cap && bl_link_prefix_valid(value)) copy(dst, cap, value);
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlLinkConf *c = (BlLinkConf *)ctx;
    uint32_t u = 0;
    bool b = false;
    c->read = true;
    if (!strcmp(key, "enabled")) {
        if (bl_parse_bool(value, &b)) c->enabled = b;
    } else if (!strcmp(key, "host")) {
        str_field(c->host, sizeof(c->host), value);
    } else if (!strcmp(key, "port")) {
        if (bl_parse_uint(value, 1, 65535, &u)) c->port = (uint16_t)u;
    } else if (!strcmp(key, "tls")) {
        if (bl_parse_bool(value, &b)) c->tls = b;
    } else if (!strcmp(key, "ca_file")) {
        str_field(c->ca_file, sizeof(c->ca_file), value);
    } else if (!strcmp(key, "mqtt_version")) {
        if (!strcmp(value, "auto")) c->mqtt_version = BlLinkMqtt_Auto;
        else if (!strcmp(value, "5") || !strcmp(value, "5.0")) c->mqtt_version = BlLinkMqtt_V5;
        else if (!strcmp(value, "3.1.1") || !strcmp(value, "4")) c->mqtt_version = BlLinkMqtt_V311;
    } else if (!strcmp(key, "username")) {
        str_field(c->username, sizeof(c->username), value);
    } else if (!strcmp(key, "password")) {
        str_field(c->password, sizeof(c->password), value);
    } else if (!strcmp(key, "allow_anonymous")) {
        if (bl_parse_bool(value, &b)) c->allow_anonymous = b;
    } else if (!strcmp(key, "console_id")) {
        if (bl_link_id_valid(value)) copy(c->console_id, sizeof(c->console_id), value);
    } else if (!strcmp(key, "console_name")) {
        str_field(c->console_name, sizeof(c->console_name), value);
    } else if (!strcmp(key, "topic_prefix")) {
        prefix_field(c->topic_prefix, sizeof(c->topic_prefix), value);
    } else if (!strcmp(key, "discovery_prefix")) {
        prefix_field(c->discovery_prefix, sizeof(c->discovery_prefix), value);
    } else {
        bl_kv_keep(&c->file, key, value);
    }
}

void bl_link_conf_parse(BlLinkConf *c, const char *text, size_t len)
{
    bl_kv_file_init(&c->file, c->kept, sizeof(c->kept));   // a copied BlLinkConf: its own buffer
    bl_kv_read(&c->file, text, len, on_kv, c);
}

bool bl_link_conf_may_write(const BlLinkConf *c)
{
    return bl_kv_may_rewrite(&c->file, BL_LINK_CONF_SCHEMA);
}

size_t bl_link_conf_write(const BlLinkConf *c, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
        "# Boréale: the MQTT broker every homebrew of the console shares. Written\n"
        "# by the homebrew whose screen sets it (PlayGuard's Remote access); it can\n"
        "# also be edited by hand. Keep this file private: it holds the broker's\n"
        "# password.\n"
        "schema=%u\n"
        "enabled=%d\n"
        "host=%s\n"
        "port=%u\n"
        "tls=%d\n"
        "ca_file=%s\n"
        "mqtt_version=%s\n"
        "username=%s\n"
        "password=%s\n"
        "allow_anonymous=%d\n"
        "console_id=%s\n"
        "console_name=%s\n"
        "topic_prefix=%s\n"
        "discovery_prefix=%s\n",
        (unsigned)BL_LINK_CONF_SCHEMA, c->enabled, c->host, (unsigned)c->port, c->tls, c->ca_file,
        bl_link_mqtt_name(c->mqtt_version), c->username, c->password, c->allow_anonymous, c->console_id,
        c->console_name, c->topic_prefix, c->discovery_prefix);
    if (n < 0 || (size_t)n >= cap) return 0;
    BlKvFile kept = c->file;
    kept.kept = (char *)c->kept;   // this BlLinkConf's lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, (size_t)n);
}
