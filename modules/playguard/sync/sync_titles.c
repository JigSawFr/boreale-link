// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_titles.h"

#include <string.h>

SyncTitleKind sync_title_kind_of_id(uint64_t id)
{
    if ((id >> 56) != 0x01) return SyncTitle_Homebrew;
    if (id < 0x0100000000010000ULL) return SyncTitle_System;
    return SyncTitle_Game;
}

static bool hex16(const char *p, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return true;
}

static bool word(const char *p, size_t n, const char *w)
{
    const size_t l = strlen(w);
    return n >= l && !memcmp(p, w, l) && (n == l || p[l] == ':' || p[l] == '\r');
}

void sync_titles_parse(SyncTitles *t, const char *text, size_t len)
{
    t->n = 0;
    for (size_t at = 0; text && at < len && t->n < SYNC_TITLES_MAX;) {
        const char *line = text + at;
        const char *end = memchr(line, '\n', len - at);
        const size_t n = end ? (size_t)(end - line) : len - at;
        at += n + 1;
        uint64_t id;
        if (n < 18 || line[16] != '=' || !hex16(line, &id)) continue;
        const char *k = line + 17;
        const size_t kn = n - 17;
        SyncTitleKind kind;
        if (word(k, kn, "homebrew")) kind = SyncTitle_Homebrew;
        else if (word(k, kn, "system")) kind = SyncTitle_System;
        else if (word(k, kn, "playguard")) kind = SyncTitle_PlayGuard;
        else if (word(k, kn, "game")) kind = SyncTitle_Game;
        // A kind a newer PlayGuard added: not a game, so never published by
        // its ID (told as a homebrew).
        else kind = SyncTitle_Homebrew;
        t->ids[t->n] = id;
        t->kinds[t->n] = (uint8_t)kind;
        t->n++;
    }
}

SyncTitleKind sync_titles_kind(const SyncTitles *t, uint64_t id)
{
    for (size_t i = 0; t && i < t->n; i++)
        if (t->ids[i] == id) return (SyncTitleKind)t->kinds[i];
    return sync_title_kind_of_id(id);
}

const char *sync_title_kind_name(SyncTitleKind k)
{
    switch (k) {
    case SyncTitle_Homebrew: return "homebrew";
    case SyncTitle_System:   return "system";
    case SyncTitle_PlayGuard: return "playguard";
    default:                 return "game";
    }
}

const char *sync_title_kind_label(SyncTitleKind k)
{
    return k == SyncTitle_PlayGuard ? "homebrew" : sync_title_kind_name(k);
}

const char *sync_now_name(uint64_t playing, SyncTitleKind kind, SyncNow now)
{
    if (playing) {
        switch (kind) {
        case SyncTitle_Homebrew:  return "homebrew";
        case SyncTitle_PlayGuard: return "playguard";
        case SyncTitle_System:    return "home";
        default:                  return "game";
        }
    }
    switch (now) {
    case SyncNow_PlayGuard: return "playguard";
    case SyncNow_Home:      return "home";
    case SyncNow_Suspended: return "suspended";
    default:                return NULL;
    }
}

size_t sync_titles_games_only(const SyncTitles *t, SyncAppTime *apps, size_t n, uint32_t *other_s)
{
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (sync_titles_kind(t, apps[i].app_id) == SyncTitle_Game) apps[kept++] = apps[i];
        else if (other_s) *other_s += apps[i].seconds;
    }
    return kept;
}
