// sync_conf — the remote link's settings (sd:/config/playguard/sync.conf,
// and the broker's part in sd:/config/boreale/link.conf, shared with every
// Boréale homebrew: sync_conf_from_link)
// and the small flat files PlayGuard and the agent exchange: "key=value",
// one per line, read by both processes with link/bl_kv (the agent has no
// JSON library). PlayGuard's Sync screen writes
// sync.conf; it can also be edited by hand. A key this version does not know
// is kept and written back.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_kv.h"
#include "bl_link_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

// 2: the topics moved under boreale/<id>/ (docs/sync-protocol.md); a file of
// schema 1 is read with its default prefix "playguard" taken to "boreale", and
// the old tree to clear once (legacy_prefix).
// 3: the broker's keys left for config/boreale/link.conf; a file of schema 1
// or 2 gives them to it, once, and is written again without them
// (`broker_keys`). Reading them goes with the release after the one that
// moves them (workbench's plans/boreale/suivi.md).
#define SYNC_CONF_SCHEMA   3
#define SYNC_HOST_MAX      254
#define SYNC_USER_MAX      65
#define SYNC_PASS_MAX      129
#define SYNC_PATH_MAX      128
#define SYNC_ID_LEN        8      // console id: 8 lower-case hex digits
#define SYNC_NAME_MAX      33
#define SYNC_PREFIX_MAX    33
#define SYNC_EXTRA_MAX     1024   // unknown lines kept

typedef enum {
    SyncPolicy_Ask  = 0,   // PlayGuard open: confirm on the console; the agent alone: refuse
    SyncPolicy_Auto = 1,   // apply without asking (still behind every write guard)
    SyncPolicy_Off  = 2,   // publish only, refuse every order
} SyncPolicy;

// The MQTT version spoken to the broker.
typedef enum {
    SyncMqtt_Auto = 0,   // 5.0, else 3.1.1 when the broker refuses 5.0 (then kept until the settings change)
    SyncMqtt_V5   = 1,
    SyncMqtt_V311 = 2,
} SyncMqttVersion;

// What a value belongs to, for the publish_* switches.
typedef enum {
    SyncPublish_Controls = 0,   // the parental controls and the link: always
    SyncPublish_Activity,
    SyncPublish_Games,
    SyncPublish_Icons,          // PlayGuard's alone (action/sync_icons)
    SyncPublish_Console,
    SyncPublish_Network,
    SyncPublish_Hardware,
    SyncPublish_Serial,         // needs publish_hardware too
    SyncPublish_Report,
    SyncPublish_Count
} SyncPublish;

typedef struct {
    bool       enabled;
    char       host[SYNC_HOST_MAX];
    uint16_t   port;                    // 1883; 8883 turns tls on by default
    bool       tls;
    char       ca_file[SYNC_PATH_MAX];  // PEM, for a broker with a private certificate authority
    SyncMqttVersion mqtt_version;
    char       username[SYNC_USER_MAX];
    char       password[SYNC_PASS_MAX];
    bool       allow_anonymous;         // connect without a user name
    char       console_id[SYNC_ID_LEN + 1];
    char       console_name[SYNC_NAME_MAX];
    SyncPolicy policy;
    bool       remote_timer_writes;     // orders may change the play timer (unlocking with the stored PIN)
    // What is published beside the parental controls (always): each on by
    // default but the serial number. Off: its values are null, its documents
    // cleared, its entities removed from Home Assistant.
    bool       publish_report;
    bool       publish_activity;        // play time: today, the week, the finished days, the game names
    bool       publish_games;           // the games document (the list, each one's play time)
    bool       publish_icons;           // the icons of the games played lately (icon/<app id>)
    bool       publish_console;         // power, storage, when it started (the system document)
    bool       publish_network;         // the connection, IP address, Wi-Fi (the system document)
    bool       publish_hardware;        // versions and hardware: model, firmware, Atmosphère, hekate…
    bool       publish_serial;          // the serial number, with publish_hardware (off by default)
    bool       ha_discovery;            // Home Assistant MQTT discovery
    uint16_t   poll_s;                  // state refresh, 10..300
    char       topic_prefix[SYNC_PREFIX_MAX];      // "boreale"
    char       discovery_prefix[SYNC_PREFIX_MAX];  // "homeassistant"
    // The prefix of the tree a schema-1 file published under, until the
    // engine has cleared what is left of it on the broker ("" none).
    char       legacy_prefix[SYNC_PREFIX_MAX];
    int        log_level;               // 0 errors, 1 info, 2 debug
    // The file's schema (bl_kv: absent in the file is the current one, not
    // 1), the lines it could not read, and the lines with unknown keys, kept
    // in `kept` to be written back. `file.kept` points into this struct: a
    // copy has to point it at its own `kept` (sync_conf_write does).
    BlKvFile   file;
    char       kept[SYNC_EXTRA_MAX];
    // The file had broker keys (schema 1 or 2): they go to link.conf, and the
    // file is written again without them.
    bool       broker_keys;
    // Not in the file: set by the host, which runs on an emuMMC. sync.conf is
    // shared by every system on the SD card, and each has its own parental
    // controls: an emuMMC is a device of its own (sync_conf_device_id).
    bool       emummc;
} SyncConf;

void sync_conf_defaults(SyncConf *c);
// Reads `text` over the defaults (call sync_conf_defaults first). Values out
// of range keep their default. Returns false when nothing could be read at
// all (an empty file is fine and returns true).
bool sync_conf_parse(SyncConf *c, const char *text, size_t len);
// The file's content; the length written, 0 when `cap` is too small.
size_t sync_conf_write(const SyncConf *c, char *out, size_t cap);
// Whether what was read may be written back: NULL yes, else an English
// reason. A file a newer version wrote (its schema above SYNC_CONF_SCHEMA)
// is read for the keys this one knows but never written over, by PlayGuard
// or the agent; nor is one with a line that could not be read or kept
// (writing it would lose it).
const char *sync_conf_unwritable(const SyncConf *c);
// What is still missing before connecting: NULL when ready, else an English
// reason ("no broker host" …). The UI shows its own words.
const char *sync_conf_problem(const SyncConf *c);
// 8 lower-case hex digits.
bool sync_conf_id_valid(const char *id);
// The id this system goes by on the broker and in Home Assistant (topics,
// client id, discovery): console_id, or on an emuMMC the same id with its
// first digit's top bit flipped ("5a0c3e11" -> "da0c3e11"), still 8 hex
// digits, so that a sysMMC and an emuMMC sharing one sync.conf are two
// devices. Empty when console_id is not valid yet.
void sync_conf_device_id(const SyncConf *c, char out[SYNC_ID_LEN + 1]);
// Its name: console_name; when empty, the console's nickname (System
// Settings, NULL or "" unknown) if the hardware is published, else
// "Nintendo Switch"; " (emuMMC)" added on an emuMMC. `cap` of
// SYNC_DEVICE_NAME_MAX is always enough.
#define SYNC_DEVICE_NAME_MAX (SYNC_NAME_MAX + 0x80 + 16)
void sync_conf_device_name(const SyncConf *c, const char *nickname, char *out, size_t cap);
// "ask" / "auto" / "off".
const char *sync_policy_name(SyncPolicy p);
// The category is published (SyncPublish_Controls always).
bool        sync_conf_publishes(const SyncConf *c, SyncPublish p);
// "controls", "activity", "games", "icons", "console", "network", "hardware", "serial", "report".
const char *sync_publish_name(SyncPublish p);
// "auto" / "5" / "3.1.1", as sync.conf writes it.
const char *sync_mqtt_version_name(SyncMqttVersion v);

// The broker's part of the settings, which config/boreale/link.conf holds
// for every Boréale homebrew (bl_link_conf): enabled, the broker, TLS, the
// MQTT version, the credentials, the console's id and name, the prefixes.
// link.conf holds them alone: sync.conf's own (schema 1 or 2) are moved to
// it once (`broker_keys`). `l` keeps the lines it read and does not know.
void sync_conf_to_link(const SyncConf *c, BlLinkConf *l);
void sync_conf_from_link(SyncConf *c, const BlLinkConf *l);

#ifdef __cplusplus
}
#endif
