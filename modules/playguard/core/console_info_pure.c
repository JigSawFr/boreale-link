// The parsing half of console_info.h: plain C, shared by PlayGuard, the agent
// and the desktop build, host-tested (tests/console_info).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "console_info.h"

#include <stdio.h>
#include <string.h>

void console_live_init(ConsoleLive *out)
{
    memset(out, 0, sizeof(*out));
    out->battery_pct = -1;
    out->charger = -1;
    out->docked = -1;
    out->temperature_c = -1000;
    out->network = -1;
    out->wifi_bars = -1;
}

ConsoleModel console_model_of(int product_model)
{
    switch (product_model) {
    case 1: return ConsoleModel_Erista;   // Nx
    case 3: return ConsoleModel_Mariko;   // Iowa
    case 4: return ConsoleModel_Lite;     // Hoag
    case 6: return ConsoleModel_Oled;     // Aula
    case 2:                               // Copper
    case 5: return ConsoleModel_Other;    // Calcio
    default: return ConsoleModel_Unknown;
    }
}

const char *console_model_name(ConsoleModel m)
{
    static const char *const NAMES[] = { NULL, "erista", "mariko", "lite", "oled", "other" };
    return (unsigned)m < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[m] : NULL;
}

const char *console_region_name(int region)
{
    static const char *const NAMES[] = { "jpn", "usa", "eur", "aus", "htk", "chn" };
    return region >= 0 && (size_t)region < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[region] : NULL;
}

const char *console_charger_name(int charger)
{
    static const char *const NAMES[] = { "none", "official", "low_power", "not_supported" };
    return charger >= 0 && (size_t)charger < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[charger] : NULL;
}

const char *console_network_name(int network)
{
    static const char *const NAMES[] = { "none", "wifi", "ethernet" };
    return network >= 0 && (size_t)network < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[network] : NULL;
}

void console_ip_text(uint32_t addr, char out[16])
{
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

bool console_hekate_version(const uint8_t *bytes, size_t len, char out[16])
{
    const size_t limit = len < 1024 ? len : 1024;
    for (size_t i = 0; i + 7 <= limit; i++) {
        if (memcmp(bytes + i, "ICTC", 4) != 0) continue;
        int v[3];
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            v[k] = bytes[i + 4 + k] - '0';
            ok = ok && v[k] >= 0 && v[k] <= 40;
        }
        if (!ok) continue;
        snprintf(out, 16, "%d.%d.%d", v[0], v[1], v[2]);
        return true;
    }
    return false;
}

static size_t read_head(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

// `sd_root` and `rest` joined with one '/'.
static void join(char *out, size_t cap, const char *sd_root, const char *rest)
{
    const size_t n = strlen(sd_root);
    snprintf(out, cap, "%s%s%s", sd_root, n && sd_root[n - 1] == '/' ? "" : "/", rest);
}

bool console_hekate(const char *sd_root, char out[16])
{
    char path[256];
    join(path, sizeof(path), sd_root, "bootloader/update.bin");
    uint8_t head[1024];
    const size_t n = read_head(path, head, sizeof(head));
    return n && console_hekate_version(head, n, out);
}
