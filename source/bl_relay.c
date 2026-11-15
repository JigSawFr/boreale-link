// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_relay.h"

#include <stdio.h>
#include <string.h>

#include "bl_app.h"

// The levels of the console itself, published by the agent only.
static const char *const RESERVED[] = {"availability", "capabilities", "system", "icon", "integration", "notices"};

bool bl_relay_module_ok(const char *module)
{
    if (!bl_app_module_valid(module)) return false;
    for (size_t i = 0; i < sizeof(RESERVED) / sizeof(RESERVED[0]); i++)
        if (!strcmp(module, RESERVED[i])) return false;
    return true;
}

static bool sub_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
           ch == '.';
}

bool bl_relay_sub_ok(const char *sub)
{
    if (!sub || !*sub || strlen(sub) >= BL_RELAY_SUB_MAX) return false;
    const char *level = sub;
    for (const char *p = sub;; p++) {
        if (*p == '/' || *p == '\0') {
            const size_t n = (size_t)(p - level);
            if (n == 0) return false;                                  // "", leading, trailing or doubled '/'
            if (n == 1 && level[0] == '.') return false;
            if (n == 2 && level[0] == '.' && level[1] == '.') return false;
            if (*p == '\0') return true;
            level = p + 1;
        } else if (!sub_char(*p)) {
            return false;
        }
    }
}

bool bl_relay_topic(const char *module, const char *sub, char *out, size_t cap)
{
    if (!bl_relay_module_ok(module) || !bl_relay_sub_ok(sub)) return false;
    const int n = snprintf(out, cap, "%s/%s", module, sub);
    return n > 0 && (size_t)n < cap;
}

bool bl_relay_split(const char *rel, char *module, size_t module_cap, const char **sub)
{
    if (!rel) return false;
    const char *slash = strchr(rel, '/');
    if (!slash) return false;
    const size_t n = (size_t)(slash - rel);
    if (n == 0 || n >= module_cap || n >= BL_APP_MODULE_MAX) return false;
    memcpy(module, rel, n);
    module[n] = '\0';
    if (!bl_relay_module_ok(module) || !bl_relay_sub_ok(slash + 1)) return false;
    if (sub) *sub = slash + 1;
    return true;
}

void bl_relay_queue_init(BlRelayQueue *q)
{
    memset(q, 0, sizeof(*q));
}

bool bl_relay_queue_push(BlRelayQueue *q, const char *sub, const void *payload, size_t len, bool retained)
{
    if (q->count >= BL_RELAY_QUEUE || !bl_relay_sub_ok(sub) || len > BL_RELAY_ORDER_MAX || (len && !payload)) {
        q->refused++;
        return false;
    }
    BlRelayOrder *o = &q->items[(q->head + q->count) % BL_RELAY_QUEUE];
    memset(o, 0, sizeof(*o));
    snprintf(o->sub, sizeof(o->sub), "%s", sub);
    if (len) memcpy(o->payload, payload, len);
    o->len = (uint16_t)len;
    o->retained = retained ? 1 : 0;
    q->count++;
    return true;
}

bool bl_relay_queue_pop(BlRelayQueue *q, BlRelayOrder *out)
{
    if (q->count == 0) return false;
    *out = q->items[q->head];
    q->head = (uint8_t)((q->head + 1) % BL_RELAY_QUEUE);
    q->count--;
    return true;
}

size_t bl_relay_encode_publish(const char *sub, const void *payload, size_t len, bool retained, void *out, size_t cap)
{
    if (!bl_relay_sub_ok(sub) || len > BL_RELAY_PAYLOAD_MAX || (len && !payload)) return 0;
    const size_t sub_len = strlen(sub);
    const size_t total = 1 + sub_len + 1 + len;
    if (!out || total > cap) return 0;
    uint8_t *b = (uint8_t *)out;
    b[0] = retained ? 1 : 0;
    memcpy(b + 1, sub, sub_len + 1);
    if (len) memcpy(b + 1 + sub_len + 1, payload, len);
    return total;
}

bool bl_relay_decode_order(const void *buf, size_t len, const char **sub, const void **payload, size_t *payload_len,
                           bool *retained)
{
    if (!buf || len < 3) return false;
    const char *b = (const char *)buf;
    const char *s = b + 1;
    const size_t max_sub = len - 1;
    const size_t sub_len = strnlen(s, max_sub);
    if (sub_len == max_sub || !bl_relay_sub_ok(s)) return false;   // no NUL, or a topic that is not allowed
    const size_t n = len - 1 - sub_len - 1;
    if (n > BL_RELAY_ORDER_MAX) return false;
    if (sub) *sub = s;
    if (payload) *payload = s + sub_len + 1;
    if (payload_len) *payload_len = n;
    if (retained) *retained = (b[0] & 1) != 0;
    return true;
}
