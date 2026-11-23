// bl_agent_conf — the agent's own settings, sd:/config/boreale/agent.conf: what
// it may ask of the network beyond its broker. Both checks are off until
// someone turns them on, so an agent that only talks to the broker stays that.
//
//     schema=1
//     check_versions=1            read the newest Atmosphère, hekate, sys-patch, firmware and
//                                 boreale-ha (GitHub, switchbrew) every 12 h → latest.conf
//     check_clock=1               compare the clocks with a time server every 2 h → latest.conf
//     ntp_server=pool.ntp.org     the server for check_clock
//
// A bl_kv file with a `schema`: keys a version does not know are kept, a file
// of a newer schema read but never written over. No allocation.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bl_kv.h"
#include "bl_link_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_AGENT_CONF_FILE   BL_LINK_CONF_DIR "/agent.conf"   // under the SD card's root
#define BL_AGENT_CONF_SCHEMA 1
#define BL_AGENT_NTP_MAX     64
#define BL_AGENT_NTP_DEFAULT "pool.ntp.org"
#define BL_AGENT_CONF_KEPT_MAX 256

typedef struct {
    bool     check_versions;
    bool     check_clock;
    char     ntp_server[BL_AGENT_NTP_MAX];
    BlKvFile file;
    char     kept[BL_AGENT_CONF_KEPT_MAX];
} BlAgentConf;

void   bl_agent_conf_defaults(BlAgentConf *c);
void   bl_agent_conf_parse(BlAgentConf *c, const char *text, size_t len);
bool   bl_agent_conf_may_write(const BlAgentConf *c);
size_t bl_agent_conf_write(const BlAgentConf *c, char *out, size_t cap);
// Whether a host name can go to a time server's lookup: letters, digits, '.', '-'.
bool   bl_agent_ntp_valid(const char *host);

#ifdef __cplusplus
}
#endif
