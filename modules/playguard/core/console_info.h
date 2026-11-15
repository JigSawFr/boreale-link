// console_info — what else the console says about itself, for the remote
// link's system document (sync/sync_system.h): its model, name and region,
// its power and network, how long it has been on, the space left, hekate on
// the SD card, and the serial number PRODINFO holds. Each value is "unknown"
// (-1, "", ConsoleModel_Unknown) when its call fails: nothing is guessed.
// The readers are console_info.c (libnx, PlayGuard and the agent) and
// sim/sim_console.c (the desktop build); the parsing below is plain C
// (console_info_pure.c, host-tested in tests/console_info).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ConsoleModel_Unknown = 0,
    ConsoleModel_Erista,   // the first Switch (Nx)
    ConsoleModel_Mariko,   // the 2019 revision (Iowa)
    ConsoleModel_Lite,     // Switch Lite (Hoag)
    ConsoleModel_Oled,     // Switch OLED (Aula)
    ConsoleModel_Other,    // a development model (Copper, Calcio)
} ConsoleModel;

// What changes while it runs: read at each state document.
typedef struct {
    ConsoleModel model;
    char         nickname[0x80];   // System Settings › System › Console nickname
    char         region[4];        // "jpn", "usa", "eur", "aus", "htk" (Hong Kong, Taiwan, Korea), "chn"
    int          battery_pct;      // 0–100
    int          charger;          // 0 none, 1 enough power (the official charger, the dock), 2 low power, 3 not supported
    int          docked;           // 1 docked (boost performance mode), 0 handheld
    int          temperature_c;    // the internal sensor, whole degrees; -1000 unknown
    int          network;          // 0 none, 1 Wi-Fi, 2 ethernet
    char         ip[16];           // "192.168.1.20"
    int          wifi_bars;        // 0–3, Wi-Fi only
    char         ssid[33];
    uint64_t     uptime_s;         // since the console started (not since it woke up); 0 unknown
} ConsoleLive;

// Space on a storage, in MiB; -1 unknown.
typedef struct {
    int64_t free_mb, total_mb;
} ConsoleSpace;

void console_live_init(ConsoleLive *out);   // every field unknown
void console_live_read(ConsoleLive *out);
// The console nickname alone (System Settings), "" unknown: the console's
// name in Home Assistant when sync.conf names none.
void console_nickname_read(char out[0x80]);
// The SD card's, and the NAND's user partition (the system in use: the
// emuMMC's on an emuMMC).
void console_space_read(ConsoleSpace *sd, ConsoleSpace *nand);

// ---- parsing (console_info_pure.c) ----

ConsoleModel console_model_of(int product_model);   // SetSysProductModel
const char  *console_model_name(ConsoleModel m);    // "erista" …; NULL unknown
const char  *console_region_name(int region);       // SetRegion; NULL unknown
const char  *console_charger_name(int charger);     // "none", "official", "low_power", "not_supported"; NULL unknown
const char  *console_network_name(int network);     // "none", "wifi", "ethernet"; NULL unknown
// The IPv4 address nifm gives (network byte order in memory) as text.
void         console_ip_text(uint32_t addr, char out[16]);

// hekate's version in bootloader/update.bin: the "ICTC" magic of its
// ipl_ver, then one byte per number ('0' + n), within the first kilobyte.
bool console_hekate_version(const uint8_t *bytes, size_t len, char out[16]);
// The same from `sd_root`/bootloader/update.bin.
bool console_hekate(const char *sd_root, char out[16]);

#ifdef __cplusplus
}
#endif
