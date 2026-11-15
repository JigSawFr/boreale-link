// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "sync_watch.h"

#include <string.h>

#include "bl_json.h"
#include "sync_titles.h"

static void notice(SyncNotice out[SYNC_NOTICE_MAX], size_t *n, const char *type, uint64_t app_id, int kind,
                   int minutes_left, const char *blocked_by)
{
    if (*n >= SYNC_NOTICE_MAX) return;
    out[(*n)++] = (SyncNotice){ type, app_id, kind, minutes_left, blocked_by };
}

size_t sync_watch_step(SyncWatch *w, const SyncSnapshot *s, SyncNotice out[SYNC_NOTICE_MAX])
{
    const char *blocked_by = sync_blocked_by(s);
    const bool blocked = blocked_by != NULL;
    // A game is played only while the play log says so; with PlayGuard in
    // front (its own snapshot) nobody plays.
    const uint64_t playing = s->now_playing;
    const int kind = (int)s->now_playing_kind;
    int64_t left = 0;
    const bool left_ok = sync_remaining_min(s, &left);
    const bool low = left_ok && !blocked && left <= SYNC_WARN_MIN;

    size_t n = 0;
    if (w->seen) {
        if (w->playing && w->playing != playing) notice(out, &n, "play_stopped", w->playing, w->playing_kind, -1, NULL);
        if (playing && playing != w->playing) notice(out, &n, "play_started", playing, kind, -1, NULL);
        if (low && !w->warned && playing) notice(out, &n, "time_warning", playing, kind, (int)left, NULL);
        if (blocked && !w->blocked)
            notice(out, &n, "play_blocked", playing ? playing : w->playing, playing ? kind : w->playing_kind, -1, blocked_by);
    }
    w->seen = true;
    w->playing = playing;
    w->playing_kind = kind;
    w->blocked = blocked;
    // Given once per stretch: the warning comes again after extra time takes
    // the time left back above it, or on another day.
    if (low && (playing || w->warned)) w->warned = true;
    else if (!low && !blocked) w->warned = false;
    return n;
}

size_t sync_notice_build(const char *source, uint64_t ts, const SyncNotice *n, char *out, size_t cap)
{
    BlJson j;
    bl_json_init(&j, out, cap);
    bl_json_obj(&j);
    bl_json_kint(&j, "schema", SYNC_SCHEMA);
    bl_json_kstr(&j, "source", source ? source : "agent");
    bl_json_kint(&j, "ts", (int64_t)ts);
    bl_json_kstr(&j, "event_type", n->event_type);
    bl_json_key(&j, "app_id");
    if (n->app_id) bl_json_hex64(&j, n->app_id);
    else bl_json_null(&j);
    bl_json_kstr_or_null(&j, "kind", n->app_id ? sync_title_kind_label((SyncTitleKind)n->kind) : NULL);
    bl_json_kint_or_null(&j, "minutes_left", n->minutes_left >= 0, n->minutes_left);
    bl_json_kstr_or_null(&j, "blocked_by", n->blocked_by);
    bl_json_obj_end(&j);
    return bl_json_end(&j);
}
