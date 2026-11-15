// PlayGuard — the service-free part of the play data layer (see playstats_logic.h).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "playstats_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool playstats_convert(const PdmPlayEvent *p, PlayLogEvent *e)
{
    e->app_id    = 0;
    e->ts_user   = p->timestamp_user;
    e->ts_steady = p->timestamp_steady;
    e->uid[0] = e->uid[1] = 0;

    if (p->play_event_type == PdmPlayEventType_Account) {
        // 0: the account is opened (picked in the game), 1: closed. The u32
        // halves are stored swapped in each u64, as for the ProgramId below.
        const u8 type = p->event_data.account.type;
        if (type > 1) return false;
        const u32 *w = p->event_data.account.uid;
        e->uid[0] = ((u64)w[0] << 32) | w[1];
        e->uid[1] = ((u64)w[2] << 32) | w[3];
        e->kind   = type == 0 ? PlayLogEv_AccountOpen : PlayLogEv_AccountClose;
        return true;
    }
    if (p->play_event_type == PdmPlayEventType_PowerStateChange) {   // sleep, wake, shutdown
        e->kind = PlayLogEv_Away;
        return true;
    }
    if (p->play_event_type != PdmPlayEventType_Applet) return false;

    const u8 type = p->event_data.applet.event_type;
    if (p->event_data.applet.applet_id != AppletId_application) {
        // The HOME menu taking the focus: no game has it any more (this is
        // what ends a session whose game crashed without "out of focus").
        if (p->event_data.applet.applet_id == AppletId_SystemAppletMenu && type == PdmAppletEventType_InFocus) {
            e->kind = PlayLogEv_Away;
            return true;
        }
        return false;
    }
    // Same filter as the system's own play statistics.
    if (p->event_data.applet.log_policy != PdmPlayLogPolicy_All) return false;
    // The two halves of the ProgramId are stored swapped.
    e->app_id = ((u64)p->event_data.applet.program_id[0] << 32) | p->event_data.applet.program_id[1];
    switch (type) {
        case PdmAppletEventType_InFocus:
            e->kind = PlayLogEv_Focus;
            return true;
        case PdmAppletEventType_OutOfFocus:
        case PdmAppletEventType_OutOfFocus4:
            e->kind = PlayLogEv_Unfocus;   // the HOME menu in front: it waits in the background
            return true;
        case PdmAppletEventType_Exit:
        case PdmAppletEventType_Exit5:
        case PdmAppletEventType_Exit6:
            e->kind = PlayLogEv_Exit;
            return true;
        case PdmAppletEventType_Launch:   // a new start: whatever was in focus is over,
            e->app_id = 0;                // and the accounts the previous game had open
            e->kind   = PlayLogEv_Launch;
            return true;
        default:
            return false;
    }
}

// A whole chunk, not the first old entry: a clock set back makes the user
// times go back and forth. Only the last week matters; the log holds years.
s32 playstats_first_recent_index(PlayEventQuery query, s32 start, s32 total, u64 since, PdmPlayEvent *chunk)
{
    s32 hi = start + total;   // one past the newest entry
    while (hi > start) {
        const s32 lo = hi - PLAYSTATS_EVENT_CHUNK > start ? hi - PLAYSTATS_EVENT_CHUNK : start;
        s32 got = 0;
        if (R_FAILED(query(lo, chunk, hi - lo, &got)) || got <= 0) return start;   // all of it, then
        bool recent = false;
        for (s32 i = 0; i < got && !recent; i++) recent = chunk[i].timestamp_user >= since;
        if (!recent) return hi;
        hi = lo;
    }
    return start;
}

Result playstats_collect_events(PlayEventQuery query, s32 start, s32 total, u64 since, size_t max,
                                PlayLogEvent **events, size_t *count, bool *capped)
{
    *events = NULL;
    *count  = 0;
    *capped = false;
    if (total <= 0 || max == 0) return 0;
    PdmPlayEvent *chunk = (PdmPlayEvent *)malloc(sizeof(PdmPlayEvent) * PLAYSTATS_EVENT_CHUNK);
    if (!chunk) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    s32 index = playstats_first_recent_index(query, start, total, since, chunk);
    // The clock set back months: every entry written since looks recent.
    // Only the newest, so the read stays bounded (and today stays right).
    if ((u64)(start + total - index) > max) {
        index   = (s32)((u64)start + (u64)total - max);
        *capped = true;
    }
    s32 remaining = start + total - index;
    size_t cap = 0;
    Result rc = 0;
    while (remaining > 0 && R_SUCCEEDED(rc)) {
        s32 got = 0;
        rc = query(index, chunk, remaining < PLAYSTATS_EVENT_CHUNK ? remaining : PLAYSTATS_EVENT_CHUNK, &got);
        if (R_FAILED(rc) || got <= 0) break;
        index += got;
        remaining -= got;
        for (s32 i = 0; i < got; i++) {
            PlayLogEvent e;
            if (!playstats_convert(&chunk[i], &e) || e.ts_user < since) continue;
            if (*count == max) {   // more than asked for after all (the log grew meanwhile)
                *capped   = true;
                remaining = 0;
                break;
            }
            if (*count == cap) {
                size_t grown = cap ? cap * 2 : 1024;
                if (grown > max) grown = max;
                PlayLogEvent *bigger = (PlayLogEvent *)realloc(*events, grown * sizeof(PlayLogEvent));
                if (!bigger) {
                    rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
                    break;
                }
                *events = bigger;
                cap = grown;
            }
            (*events)[(*count)++] = e;
        }
    }
    free(chunk);
    return rc;
}

// The whole log, a chunk at a time: only the IDs are kept, never the events.
size_t playstats_logged_games(PlayEventQuery query, s32 start, s32 total, u64 *ids, size_t max, Result *rc)
{
    *rc = 0;
    size_t n = 0;
    if (total <= 0 || max == 0) return 0;
    PdmPlayEvent *chunk = (PdmPlayEvent *)malloc(sizeof(PdmPlayEvent) * PLAYSTATS_EVENT_CHUNK);
    if (!chunk) {
        *rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        return 0;
    }
    s32 index = start, remaining = total;
    while (remaining > 0 && n < max) {
        s32 got = 0;
        *rc = query(index, chunk, remaining < PLAYSTATS_EVENT_CHUNK ? remaining : PLAYSTATS_EVENT_CHUNK, &got);
        if (R_FAILED(*rc) || got <= 0) break;
        index += got;
        remaining -= got;
        for (s32 i = 0; i < got && n < max; i++) {
            PlayLogEvent e;
            if (!playstats_convert(&chunk[i], &e) || e.kind != PlayLogEv_Focus || !e.app_id) continue;
            bool seen = false;
            for (size_t k = 0; k < n && !seen; k++) seen = ids[k] == e.app_id;
            if (!seen) ids[n++] = e.app_id;
        }
    }
    free(chunk);
    return n;
}

void playstats_add_played(PlayStats *out, const u64 *ids, size_t count, PlayTotalsQuery query, void *ctx)
{
    for (size_t i = 0; i < count; i++) {
        bool seen = false;
        for (u32 k = 0; k < out->count && !seen; k++) seen = out->games[k].app_id == ids[i];
        if (seen) continue;
        GameStat g;
        memset(&g, 0, sizeof(g));
        g.app_id = ids[i];
        if (!query(ctx, ids[i], &g)) continue;
        // Full: one more played game is enough to say the list is cut.
        if (out->count == PLAYSTATS_MAX) {
            out->truncated = true;
            return;
        }
        out->games[out->count++] = g;
    }
}

void playnames_use_language(PlayNames *m, u8 lang)
{
    if (m->lang == lang) return;
    m->lang  = lang;
    m->count = 0;
}

const char *playnames_find(const PlayNames *m, u64 id)
{
    for (u32 i = 0; i < m->count; i++)
        if (m->e[i].id == id) return m->e[i].name;
    return NULL;
}

void playnames_add(PlayNames *m, u64 id, const char *name)
{
    if (!name[0] || playnames_find(m, id) || m->count == PLAYSTATS_MAX) return;
    m->e[m->count].id = id;
    nacp_copy_utf8(m->e[m->count].name, sizeof(m->e[m->count].name), name);
    m->count++;
}

void playnames_seed(PlayNames *m, const PlayStats *known)
{
    if (!known || !known->names_lang) return;
    if (m->lang == 0 && m->count == 0) m->lang = known->names_lang;   // the first read of the run
    if (m->lang != known->names_lang) return;   // this run's names, in the language of now
    for (u32 i = 0; i < known->count && i < PLAYSTATS_MAX; i++) {
        const GameStat *g = &known->games[i];
        if (nacp_utf8_valid(g->name, strnlen(g->name, sizeof(g->name)))) playnames_add(m, g->app_id, g->name);
    }
}
