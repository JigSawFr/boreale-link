// sync_system — the system document (docs/sync-protocol.md, `system`): what
// the console is and how it is doing, beside the parental controls the state
// document carries: versions, model, name, region, which system runs, the
// serial number (only when sync.conf's publish_serial says so), the space
// left, power, network and when it started. Built by PlayGuard and by the
// agent from the same readers (core/console_info.h); unknown values are null.
// The agent adds `latest` (the newest Atmosphère, hekate, firmware, sys-patch and
// integration it saw) and `clock` (how far the clocks are from a time server)
// when its checks are on: a document without them is a console nobody checks.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bl_latest.h"
#include "../core/console_info.h"
#include "core/sysinfo.h"
#include "sync_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

// How the agent is doing, said in the system document (only an agent has one).
typedef struct {
    uint32_t uptime_s;            // since it started
    int32_t  heap_used;           // bytes, -1 unknown
    int32_t  heap_total;          // its heap: what the module's work and the relay must fit in
    uint8_t  relay_protocol;      // bl:link's
    uint8_t  ipc_protocol;        // pg:agent's
    bool     module;              // PlayGuard's module runs (capabilities lists it)
} SyncAgentHealth;

typedef struct {
    uint64_t            ts;              // POSIX seconds; 0 unknown (then no boot_ts)
    const SysInfo      *sys;
    const ConsoleLive  *live;
    const ConsoleSpace *sd, *nand;       // NULL: not read yet
    const char         *hekate;          // "6.5.4"; NULL or "" unknown
    const char         *app_version;     // PlayGuard's; NULL unknown
    const char         *agent_version;   // NULL: no agent
    const char         *serial;          // NULL: not published (or not known)
    const SyncConf     *conf;            // what is published (publish_*); NULL: everything
    const SyncAgentHealth *health;       // NULL: not an agent
    const BlLatest     *latest;          // the agent's version and clock checks (latest.conf); NULL: none made
} SyncSystem;

size_t sync_system_build(const SyncSystem *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
