// sync_discovery — Home Assistant's native MQTT discovery for one console:
// one retained, device-based payload on
// `<discovery_prefix>/device/playguard_<id>/config`, built from the entity
// table (sync_entities.c). Needs Home Assistant 2026.5 or later: an older one
// knows no MQTT `time` platform and refuses the whole payload. Orders that need
// remote_timer_writes are only announced while it is on, and none while
// PlayGuard is read-only; those not offered are sent as their platform alone
// (`{"p":"number"}`), which removes them from Home Assistant. The device is
// the console as Nintendo names it: its model, and the system it runs as its
// software (`23.0.1 · Atmosphère 1.10.1`); PlayGuard is the origin.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include "../core/console_info.h"
#include "sync_apply.h"
#include "sync_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the console is, for the device Home Assistant shows.
typedef struct {
    ConsoleModel model;         // ConsoleModel_Unknown: unknown
    uint32_t     hos_version;   // MAKEHOSVERSION layout; 0 unknown
    uint32_t     ams_version;   // Atmosphère's, the same layout; 0 not running or unknown
    char         nickname[0x80];   // System Settings' console nickname, its name when sync.conf names none ("" unknown)
} SyncDevice;

// From what the system document reads (either may be NULL: unknown).
void sync_device_of(const SysInfo *sys, const ConsoleLive *live, SyncDevice *out);

typedef struct {
    const SyncConf   *conf;
    const char       *sw_version;   // PlayGuard's (the origin's)
    bool              read_only;
    const char      (*profiles)[SYNC_PROFILE_MAX];   // the profile select's options (none: no select)
    size_t            n_profiles;
    const SyncDevice *device;       // NULL: unknown
} SyncDiscovery;

size_t sync_discovery_topic(const SyncConf *conf, char *out, size_t cap);
size_t sync_discovery_build(const SyncDiscovery *d, char *out, size_t cap);

// `<topic_prefix>/<device id>` (sync_conf_device_id; no trailing slash): the
// console's own topics are under it, PlayGuard's under SYNC_MODULE/.
size_t sync_topic_base(const SyncConf *conf, char *out, size_t cap);

// PlayGuard's subtree under the console's (docs/sync-protocol.md).
#define SYNC_MODULE "playguard"
// What the console is, whatever homebrew publishes it: its availability, the
// capabilities, the system document, the game icons, the integration's
// document and every homebrew's shared bell (notices) stay at the console's
// level; the rest is PlayGuard's.
bool sync_topic_is_device(const char *sub);
// `sub` under the base, at the console's level or under SYNC_MODULE/ as
// sync_topic_is_device says: the state under playguard/, the system document
// at the base.
size_t sync_topic_sub(const char *sub, char *out, size_t cap);
// The whole topic: sync_topic_base, then sync_topic_sub.
size_t sync_topic(const SyncConf *conf, const char *sub, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
