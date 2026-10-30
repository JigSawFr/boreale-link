// bl_link_conf — the broker's settings the Boréale homebrews share,
// sd:/config/boreale/link.conf: where the broker is, how to reach it, who
// the console is on it and under which prefixes. Set once, read by every
// homebrew (and their sysmodules); each keeps its own keys in its own file.
// A bl_kv file with a `schema`: the keys a version does not know are kept
// and written back, a file of a newer schema is read but never written over.
//
//     BlLinkConf c;
//     bl_link_conf_defaults(&c);
//     if (read_file(BL_LINK_CONF_FILE, text, &len)) bl_link_conf_parse(&c, text, len);
//     …
//     if (bl_link_conf_may_write(&c) && (n = bl_link_conf_write(&c, out, sizeof(out)))) save(out, n);
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bl_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

// The file, under the SD card's root, and its certificate authority's
// usual place beside it.
#define BL_LINK_CONF_DIR    "config/boreale"
#define BL_LINK_CONF_FILE   BL_LINK_CONF_DIR "/link.conf"
#define BL_LINK_CONF_CA     BL_LINK_CONF_DIR "/ca.pem"

#define BL_LINK_CONF_SCHEMA 1

#define BL_LINK_HOST_MAX    254
#define BL_LINK_USER_MAX    65
#define BL_LINK_PASS_MAX    129
#define BL_LINK_PATH_MAX    128
#define BL_LINK_ID_LEN      8      // the console id: 8 lower-case hex digits
#define BL_LINK_NAME_MAX    33
#define BL_LINK_PREFIX_MAX  33
#define BL_LINK_KEPT_MAX    1024   // the lines of keys this version does not know

// The MQTT version spoken to the broker.
typedef enum {
    BlLinkMqtt_Auto = 0,   // 5.0, else 3.1.1 when the broker refuses 5.0
    BlLinkMqtt_V5   = 1,
    BlLinkMqtt_V311 = 2,
} BlLinkMqtt;

typedef struct {
    bool       enabled;                         // the link is on
    char       host[BL_LINK_HOST_MAX];
    uint16_t   port;                            // 1883; 8883 is TLS's
    bool       tls;
    char       ca_file[BL_LINK_PATH_MAX];       // PEM, for a broker with a private certificate authority
    BlLinkMqtt mqtt_version;
    char       username[BL_LINK_USER_MAX];
    char       password[BL_LINK_PASS_MAX];
    bool       allow_anonymous;                 // connect without a user name
    char       console_id[BL_LINK_ID_LEN + 1];  // random, made once; "" not yet
    char       console_name[BL_LINK_NAME_MAX];  // "" the console's own
    char       topic_prefix[BL_LINK_PREFIX_MAX];      // "boreale"
    char       discovery_prefix[BL_LINK_PREFIX_MAX];  // "homeassistant"
    bool       read;                            // the file had at least one line read
    BlKvFile   file;                            // its schema, what it skipped, the lines kept
    char       kept[BL_LINK_KEPT_MAX];
} BlLinkConf;

void bl_link_conf_defaults(BlLinkConf *c);
// A BlLinkConf may be copied by value.
// Reads `text` over the defaults (call bl_link_conf_defaults first): a value
// out of range keeps its default, an unknown key is kept for the round trip.
void bl_link_conf_parse(BlLinkConf *c, const char *text, size_t len);
// Whether what was read may be written back (bl_kv_may_rewrite): not a file
// of a newer schema, nor one with a line skipped or kept lines lost.
bool bl_link_conf_may_write(const BlLinkConf *c);
// The file's content, the keys kept after its own; the length written, 0
// when `cap` is too small.
size_t bl_link_conf_write(const BlLinkConf *c, char *out, size_t cap);

// 8 lower-case hex digits.
bool bl_link_id_valid(const char *id);
// A topic level we put our own names under: letters, digits, '_' and '-'.
bool bl_link_prefix_valid(const char *s);
// "auto" / "5" / "3.1.1", as the file writes it.
const char *bl_link_mqtt_name(BlLinkMqtt v);

#ifdef __cplusplus
}
#endif
