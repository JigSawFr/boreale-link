// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_agent_conf.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void bl_agent_conf_defaults(BlAgentConf *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ntp_server, sizeof(c->ntp_server), "%s", BL_AGENT_NTP_DEFAULT);
    bl_kv_file_init(&c->file, c->kept, sizeof(c->kept));
    c->file.schema = BL_AGENT_CONF_SCHEMA;
}

bool bl_agent_ntp_valid(const char *host)
{
    if (!host || !*host || strlen(host) >= BL_AGENT_NTP_MAX) return false;
    for (const char *c = host; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') return false;
    return true;
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlAgentConf *c = (BlAgentConf *)ctx;
    bool b;
    if (!strcmp(key, "check_versions")) {
        if (bl_parse_bool(value, &b)) c->check_versions = b;
    } else if (!strcmp(key, "check_clock")) {
        if (bl_parse_bool(value, &b)) c->check_clock = b;
    } else if (!strcmp(key, "ntp_server")) {
        if (bl_agent_ntp_valid(value)) snprintf(c->ntp_server, sizeof(c->ntp_server), "%s", value);
    } else {
        bl_kv_keep(&c->file, key, value);
    }
}

void bl_agent_conf_parse(BlAgentConf *c, const char *text, size_t len)
{
    bl_agent_conf_defaults(c);
    bl_kv_read(&c->file, text, len, on_kv, c);
}

bool bl_agent_conf_may_write(const BlAgentConf *c)
{
    return bl_kv_may_rewrite(&c->file, BL_AGENT_CONF_SCHEMA);
}

size_t bl_agent_conf_write(const BlAgentConf *c, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
        "# Boréale agent: what it may ask of the network besides its broker. Both checks are off by default.\n"
        "schema=%u\ncheck_versions=%d\ncheck_clock=%d\nntp_server=%s\n",
        (unsigned)BL_AGENT_CONF_SCHEMA, c->check_versions ? 1 : 0, c->check_clock ? 1 : 0, c->ntp_server);
    if (n < 0 || (size_t)n >= cap) return 0;
    BlKvFile kept = c->file;
    kept.kept = (char *)c->kept;
    return bl_kv_write_kept(&kept, out, cap, (size_t)n);
}
