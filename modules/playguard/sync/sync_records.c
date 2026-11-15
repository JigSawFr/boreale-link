// sync_records — what the agent and PlayGuard keep between runs about the
// changes they made (sync_exec.h), in the flat key=value form both read.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <string.h>

#include "sync_conf.h"
#include "sync_exec.h"

void sync_records_clear(SyncRecords *r)
{
    memset(r, 0, sizeof(*r));
    r->extra_weekday = -1;
}

size_t sync_records_write(const SyncRecords *r, char *out, size_t cap)
{
    char prev[64] = "";
    if (r->console_lock_prev_ok) {
        snprintf(prev, sizeof(prev), "%u,%u,%u,%u,%u,%u,%u", r->console_lock_prev[0], r->console_lock_prev[1],
                 r->console_lock_prev[2], r->console_lock_prev[3], r->console_lock_prev[4], r->console_lock_prev[5],
                 r->console_lock_prev[6]);
    }
    const int n = snprintf(out, cap,
                           "extra_weekday=%d\nextra_date=%s\nextra_base=%u\nextra_value=%u\n"
                           "console_lock=%d\nconsole_lock_prev=%s\nrelock_pending=%d\n",
                           r->extra_weekday, r->extra_date, r->extra_base, r->extra_value, r->console_lock, prev,
                           r->relock_pending);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

static void on_record(void *ctx, const char *key, const char *value)
{
    SyncRecords *r = (SyncRecords *)ctx;
    uint32_t u = 0;
    bool b = false;
    if (!strcmp(key, "extra_weekday")) {
        r->extra_weekday = (strcmp(value, "-1") && bl_parse_uint(value, 0, 6, &u)) ? (int8_t)u : (int8_t)-1;
    } else if (!strcmp(key, "extra_date")) {
        if (strlen(value) == 10) snprintf(r->extra_date, sizeof(r->extra_date), "%s", value);
    } else if (!strcmp(key, "extra_base")) {
        if (bl_parse_uint(value, 0, 0xFFFF, &u)) r->extra_base = (uint16_t)u;
    } else if (!strcmp(key, "extra_value")) {
        if (bl_parse_uint(value, 0, 0xFFFF, &u)) r->extra_value = (uint16_t)u;
    } else if (!strcmp(key, "console_lock")) {
        if (bl_parse_bool(value, &b)) r->console_lock = b;
    } else if (!strcmp(key, "console_lock_prev")) {
        uint16_t days[7];
        const char *p = value;
        int d = 0;
        for (; d < 7 && *p; d++) {
            char part[8];
            const char *comma = strchr(p, ',');
            const size_t n = comma ? (size_t)(comma - p) : strlen(p);
            if (n == 0 || n >= sizeof(part)) break;
            memcpy(part, p, n);
            part[n] = '\0';
            if (!bl_parse_uint(part, 0, 0xFFFF, &u)) break;
            days[d] = (uint16_t)u;
            p = comma ? comma + 1 : p + n;
        }
        r->console_lock_prev_ok = d == 7 && *p == '\0';
        if (r->console_lock_prev_ok) memcpy(r->console_lock_prev, days, sizeof(days));
    } else if (!strcmp(key, "relock_pending")) {
        if (bl_parse_bool(value, &b)) r->relock_pending = b;
    }
}

void sync_records_parse(SyncRecords *r, const char *text, size_t len)
{
    sync_records_clear(r);
    bl_kv_each(text, len, on_record, r);
    // A record without its date is no record.
    if (r->extra_weekday >= 0 && strlen(r->extra_date) != 10) r->extra_weekday = -1;
}


void sync_outcome_init(SyncOutcome *o)
{
    memset(o, 0, sizeof(*o));
    o->console_lock_after = -1;
    o->source = "remote";
}
