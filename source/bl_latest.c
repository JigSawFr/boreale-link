// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_latest.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bl_latest_defaults(BlLatest *l)
{
    memset(l, 0, sizeof(*l));
    bl_kv_file_init(&l->file, l->kept, sizeof(l->kept));
    l->file.schema = BL_LATEST_SCHEMA;
}

static bool whole_int(const char *s, long long lo, long long hi, long long *out)
{
    char *end = NULL;
    const long long v = strtoll(s, &end, 10);
    if (!end || end == s || *end != '\0' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static void set_version(char *dst, const char *value)
{
    char v[BL_LATEST_VER_MAX];
    dst[0] = '\0';
    if (bl_version_of_tag(value, v, sizeof(v))) snprintf(dst, BL_LATEST_VER_MAX, "%s", v);
}

static void on_kv(void *ctx, const char *key, const char *value)
{
    BlLatest *l = (BlLatest *)ctx;
    long long n;
    if (!strcmp(key, "checked")) l->checked = whole_int(value, 1, INT64_MAX, &n) ? n : 0;
    else if (!strcmp(key, "clock_checked")) l->clock_checked = whole_int(value, 1, INT64_MAX, &n) ? n : 0;
    else if (!strcmp(key, "clock_network")) l->clock_network = whole_int(value, INT32_MIN, INT32_MAX, &n) ? (int32_t)n : 0;
    else if (!strcmp(key, "clock_user")) l->clock_user = whole_int(value, INT32_MIN, INT32_MAX, &n) ? (int32_t)n : 0;
    else if (!strcmp(key, "clock_server")) snprintf(l->clock_server, sizeof(l->clock_server), "%s", value);
    else if (!strcmp(key, "atmosphere")) set_version(l->atmosphere, value);
    else if (!strcmp(key, "hekate")) set_version(l->hekate, value);
    else if (!strcmp(key, "firmware")) set_version(l->firmware, value);
    else if (!strcmp(key, "syspatch")) set_version(l->syspatch, value);
    else if (!strcmp(key, "integration")) set_version(l->integration, value);
    else bl_kv_keep(&l->file, key, value);
}

void bl_latest_parse(BlLatest *l, const char *text, size_t len)
{
    bl_latest_defaults(l);
    bl_kv_read(&l->file, text, len, on_kv, l);
}

bool bl_latest_may_write(const BlLatest *l)
{
    return bl_kv_may_rewrite(&l->file, BL_LATEST_SCHEMA);
}

size_t bl_latest_write(const BlLatest *l, char *out, size_t cap)
{
    // A server name with a line break would write a second key.
    char server[BL_LATEST_SERVER_MAX];
    snprintf(server, sizeof(server), "%s", l->clock_server);
    for (char *c = server; *c; c++)
        if (*c == '\n' || *c == '\r') *c = '\0';
    int n = snprintf(out, cap,
        "# Boréale: what the console runs on, as the agent last saw it.\n"
        "schema=%u\nchecked=%lld\natmosphere=%s\nhekate=%s\nfirmware=%s\nsyspatch=%s\nintegration=%s\n"
        "clock_checked=%lld\nclock_network=%d\nclock_user=%d\nclock_server=%s\n",
        (unsigned)BL_LATEST_SCHEMA, (long long)l->checked, l->atmosphere, l->hekate, l->firmware, l->syspatch,
        l->integration, (long long)l->clock_checked, (int)l->clock_network, (int)l->clock_user, server);
    if (n < 0 || (size_t)n >= cap) return 0;
    BlKvFile kept = l->file;
    kept.kept = (char *)l->kept;   // this BlLatest's lines, even copied from another one
    return bl_kv_write_kept(&kept, out, cap, (size_t)n);
}

static bool due(int64_t at, int64_t now, int64_t every)
{
    return at <= 0 || now < at || now - at >= every;
}

bool bl_latest_versions_due(const BlLatest *l, int64_t now)
{
    return due(l->checked, now, BL_LATEST_VERSIONS_EVERY_S);
}

bool bl_latest_clock_due(const BlLatest *l, int64_t now)
{
    return due(l->clock_checked, now, BL_LATEST_CLOCK_EVERY_S);
}

bool bl_version_of_tag(const char *tag, char *out, size_t cap)
{
    if (!tag) return false;
    if (*tag == 'v' || *tag == 'V') tag++;
    size_t len = 0;
    int numbers = 0;
    bool digit = false;
    for (const char *c = tag;; c++) {
        if (isdigit((unsigned char)*c)) {
            digit = true;
        } else if ((*c == '.' || *c == '\0') && digit) {
            numbers++;
            digit = false;
            if (*c == '\0') break;
        } else {
            return false;
        }
        len++;
    }
    if (numbers < 2 || numbers > 4 || len + 1 > cap || len >= BL_LATEST_VER_MAX) return false;
    memcpy(out, tag, len);
    out[len] = '\0';
    return true;
}

bool bl_tag_of_url(const char *url, char *out, size_t cap)
{
    static const char marker[] = "/releases/tag/";
    if (!url) return false;
    const char *at = strstr(url, marker);
    if (!at) return false;
    at += sizeof(marker) - 1;
    const size_t n = strcspn(at, "?#/ \r\n");
    if (n == 0 || n + 1 > cap) return false;
    memcpy(out, at, n);
    out[n] = '\0';
    return true;
}

static long next_number(const char **p)
{
    if (!**p) return 0;
    char *end = NULL;
    const long n = strtol(*p, &end, 10);
    *p = end;
    if (**p == '.') (*p)++;
    return n;
}

int bl_version_compare(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (int i = 0; i < 4; i++) {
        const long x = next_number(&a), y = next_number(&b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// "23.0.1" -> {23, 0, 1}: exactly three numbers of up to four digits.
static bool three_numbers(const char *v, size_t len, long out[3])
{
    size_t at = 0;
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            if (at >= len || v[at] != '.') return false;
            at++;
        }
        const size_t from = at;
        long n = 0;
        while (at < len && isdigit((unsigned char)v[at]) && at - from < 4) n = n * 10 + (v[at++] - '0');
        if (at == from) return false;
        out[i] = n;
    }
    return at == len;
}

static void scan_line(BlFirmwareScan *s)
{
    const char *p = s->line, *end = s->line + s->len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if ((size_t)(end - p) < 5 || strncmp(p, "| [[", 4) != 0) return;
    p += 4;
    const char *close = p;
    while (close < end && *close != ']' && *close != '|') close++;
    if (close == end) return;
    long n[3];
    if (!three_numbers(p, (size_t)(close - p), n) || (size_t)(close - p) >= sizeof(s->best)) return;
    char v[BL_LATEST_VER_MAX];
    memcpy(v, p, (size_t)(close - p));
    v[close - p] = '\0';
    if (!s->best[0] || bl_version_compare(s->best, v) < 0) memcpy(s->best, v, sizeof(v));
}

void bl_firmware_scan_init(BlFirmwareScan *s)
{
    memset(s, 0, sizeof(*s));
}

void bl_firmware_scan_feed(BlFirmwareScan *s, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (!s->over) scan_line(s);
            s->len = 0;
            s->over = false;
        } else if (s->len < sizeof(s->line)) {
            s->line[s->len++] = data[i];
        } else {
            s->over = true;   // a row is short: a long line is not one
        }
    }
}

const char *bl_firmware_scan_end(BlFirmwareScan *s)
{
    if (s->len && !s->over) scan_line(s);
    s->len = 0;
    return s->best;
}
