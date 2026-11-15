// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_icons.h"

#include <stdio.h>
#include <string.h>

void sync_icons_target(const char *host, uint16_t port, const char *base, char out[SYNC_ICONS_TARGET_LEN + 1])
{
    char key[512];
    snprintf(key, sizeof(key), "%s:%u/%s", host ? host : "", (unsigned)port, base ? base : "");
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(out, SYNC_ICONS_TARGET_LEN + 1, "%08x", (unsigned)h);
}

// The record's target line names `target`.
static bool for_target(const char *text, size_t len, const char *target)
{
    for (size_t at = 0; at < len;) {
        const char *line = text + at;
        const char *end = memchr(line, '\n', len - at);
        const size_t n = end ? (size_t)(end - line) : len - at;
        at += n + 1;
        if (n >= 7 && !memcmp(line, "target=", 7)) {
            size_t v = n - 7;
            if (v && line[7 + v - 1] == '\r') v--;
            return v == strlen(target) && !memcmp(line + 7, target, v);
        }
    }
    return false;
}

bool sync_icons_listed(const char *text, size_t len, const char *target, uint64_t app_id)
{
    if (!text || !target || !for_target(text, len, target)) return false;
    char id[17];
    snprintf(id, sizeof(id), "%016llX", (unsigned long long)app_id);
    for (size_t at = 0; at < len;) {
        const char *line = text + at;
        const char *end = memchr(line, '\n', len - at);
        const size_t n = end ? (size_t)(end - line) : len - at;
        at += n + 1;
        if (n < 17 || line[16] != '=') continue;
        bool same = true;
        for (size_t i = 0; i < 16 && same; i++) {
            char c = line[i];
            if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
            same = c == id[i];
        }
        if (same) return true;
    }
    return false;
}

size_t sync_icons_add(char *text, size_t len, size_t cap, const char *target, uint64_t app_id, uint32_t bytes)
{
    if (!text || !cap) return 0;
    if (len >= cap || !for_target(text, len, target)) {
        const int n = snprintf(text, cap, "# The game icons the agent published (docs/sync-protocol.md).\ntarget=%s\n",
                               target);
        if (n < 0 || (size_t)n >= cap) return 0;
        len = (size_t)n;
    }
    if (len && text[len - 1] != '\n') {
        if (len + 1 >= cap) return 0;
        text[len++] = '\n';
        text[len] = '\0';
    }
    const int n = snprintf(text + len, cap - len, "%016llX=%u\n", (unsigned long long)app_id, (unsigned)bytes);
    if (n < 0 || (size_t)n >= cap - len) {
        text[len] = '\0';
        return 0;
    }
    return len + (size_t)n;
}
