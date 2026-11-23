// bl_latest — what the console runs on, as the agent last saw it: the newest
// Atmosphère, hekate, sys-patch and Nintendo firmware, the newest release of
// the Home Assistant integration, and how far the console's clocks are from
// a time server. sd:/config/boreale/latest.conf, written by the agent only;
// the homebrews read it to show an update or a clock that drifted without
// asking the network themselves.
//
//     schema=1
//     checked=1792000000          when the versions were read (Unix time), 0 never
//     atmosphere=1.12.0
//     hekate=6.5.4
//     firmware=23.0.1
//     syspatch=1.6.3.0
//     integration=0.9.0           boreale-ha
//     clock_checked=1792000000    when the clocks were compared, 0 never
//     clock_network=-2            seconds the network clock is ahead of the server
//     clock_user=0                the same for the user clock
//     clock_server=europe.pool.ntp.org
//
// A bl_kv file with a `schema`: a key a version does not know is kept and
// written back, a file of a newer schema read but never written over. A key
// not read stays empty (or 0): a value is unknown, never zero by default.
// No allocation: a sysmodule writes it.
//
// The helpers below read versions out of what the sources answer: a release
// page's redirect and switchbrew's System_Versions table.
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

#define BL_LATEST_FILE      BL_LINK_CONF_DIR "/latest.conf"   // under the SD card's root
#define BL_LATEST_SCHEMA    1
#define BL_LATEST_VER_MAX   16
#define BL_LATEST_SERVER_MAX 64
#define BL_LATEST_KEPT_MAX  256
// How long a reading holds before the agent takes another.
#define BL_LATEST_VERSIONS_EVERY_S (12 * 3600)
#define BL_LATEST_CLOCK_EVERY_S    (2 * 3600)

typedef struct {
    char     atmosphere[BL_LATEST_VER_MAX];
    char     hekate[BL_LATEST_VER_MAX];
    char     firmware[BL_LATEST_VER_MAX];
    char     syspatch[BL_LATEST_VER_MAX];
    char     integration[BL_LATEST_VER_MAX];
    int64_t  checked;                       // 0 never
    int64_t  clock_checked;                 // 0 never: then the drifts below mean nothing
    int32_t  clock_network;
    int32_t  clock_user;
    char     clock_server[BL_LATEST_SERVER_MAX];
    BlKvFile file;
    char     kept[BL_LATEST_KEPT_MAX];
} BlLatest;

void bl_latest_defaults(BlLatest *l);
void bl_latest_parse(BlLatest *l, const char *text, size_t len);
bool bl_latest_may_write(const BlLatest *l);
// The file's content; the length written, 0 when `cap` is too small.
size_t bl_latest_write(const BlLatest *l, char *out, size_t cap);

// Whether a reading is due at `now` (Unix time): never taken, a day-old
// clock set back, or `every` seconds gone.
bool bl_latest_versions_due(const BlLatest *l, int64_t now);
bool bl_latest_clock_due(const BlLatest *l, int64_t now);

// A release tag as a version: "v6.5.4" -> "6.5.4", "1.12.0" -> "1.12.0", "v1.6.3.0"
// -> "1.6.3.0" (two to four numbers); false for anything else. `out` is BL_LATEST_VER_MAX.
bool bl_version_of_tag(const char *tag, char *out, size_t cap);
// The tag at the end of a release page's URL (".../releases/tag/v6.5.4" ->
// "v6.5.4"); false for any other URL.
bool bl_tag_of_url(const char *url, char *out, size_t cap);
// Numbers compared one by one, a missing one as 0: <0, 0, >0.
int  bl_version_compare(const char *a, const char *b);

// The newest firmware in switchbrew's System_Versions table (wikitext),
// read line by line as it arrives: each row has the version first
// ("| [[23.0.1]]", "| [[19.0.2]] (CHN-only)"); the highest of three numbers wins.
typedef struct {
    char    line[96];
    size_t  len;
    bool    over;                  // the current line is longer than `line`
    char    best[BL_LATEST_VER_MAX];
} BlFirmwareScan;

void bl_firmware_scan_init(BlFirmwareScan *s);
void bl_firmware_scan_feed(BlFirmwareScan *s, const char *data, size_t len);
// The last line, if the text did not end with a newline; then the newest ("" none).
const char *bl_firmware_scan_end(BlFirmwareScan *s);

#ifdef __cplusplus
}
#endif
