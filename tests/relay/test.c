// bl_relay: which modules and topics a relay session may use, and the queue of
// orders kept for it.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>

#include "bl_relay.h"
#include "bl_relay_nx.h"
#include "check.h"

static void test_modules(void)
{
    CHECK(bl_relay_module_ok("playshelf") && bl_relay_module_ok("play_guard-2"));
    CHECK(!bl_relay_module_ok("") && !bl_relay_module_ok(NULL) && !bl_relay_module_ok("Play") && !bl_relay_module_ok("a/b"));
    // The console's own levels belong to the agent.
    CHECK(!bl_relay_module_ok("availability") && !bl_relay_module_ok("capabilities") && !bl_relay_module_ok("system"));
    CHECK(!bl_relay_module_ok("icon") && !bl_relay_module_ok("integration") && !bl_relay_module_ok("notices"));
}

static void test_subs(void)
{
    CHECK(bl_relay_sub_ok("state") && bl_relay_sub_ok("downloads/12.1") && bl_relay_sub_ok("a_b-c/D"));
    CHECK(!bl_relay_sub_ok("") && !bl_relay_sub_ok(NULL));
    CHECK(!bl_relay_sub_ok("/state") && !bl_relay_sub_ok("state/") && !bl_relay_sub_ok("a//b"));
    CHECK(!bl_relay_sub_ok("..") && !bl_relay_sub_ok("a/../b") && !bl_relay_sub_ok("./a") && !bl_relay_sub_ok("a/."));
    CHECK(bl_relay_sub_ok("a/..b") && bl_relay_sub_ok("a/b..") && bl_relay_sub_ok("..a"));
    CHECK(!bl_relay_sub_ok("a/+") && !bl_relay_sub_ok("#") && !bl_relay_sub_ok("$SYS/x") && !bl_relay_sub_ok("a b"));
    char big[BL_RELAY_SUB_MAX + 1];
    memset(big, 'a', sizeof(big));
    big[BL_RELAY_SUB_MAX] = '\0';
    CHECK(!bl_relay_sub_ok(big));   // BL_RELAY_SUB_MAX bytes: no room for the NUL
    big[BL_RELAY_SUB_MAX - 1] = '\0';
    CHECK(bl_relay_sub_ok(big));
}

static void test_topics(void)
{
    char out[64];
    CHECK(bl_relay_topic("playshelf", "state", out, sizeof(out)) && !strcmp(out, "playshelf/state"));
    CHECK(!bl_relay_topic("playshelf", "../playguard/state", out, sizeof(out)));
    CHECK(!bl_relay_topic("system", "state", out, sizeof(out)));
    CHECK(!bl_relay_topic("playshelf", "state", out, 15));   // "playshelf/state" needs 16
    CHECK(bl_relay_topic("playshelf", "state", out, 16));

    char module[40];
    const char *sub = NULL;
    CHECK(bl_relay_split("playshelf/download/set", module, sizeof(module), &sub) && !strcmp(module, "playshelf") &&
          !strcmp(sub, "download/set"));
    CHECK(!bl_relay_split("playshelf", module, sizeof(module), &sub) && !bl_relay_split("/x", module, sizeof(module), &sub));
    CHECK(!bl_relay_split("system/x", module, sizeof(module), &sub) && !bl_relay_split("playshelf/", module, sizeof(module), &sub));
    CHECK(!bl_relay_split("playshelf/a/../b", module, sizeof(module), &sub) && !bl_relay_split(NULL, module, sizeof(module), &sub));
    CHECK(!bl_relay_split("playshelf/x", module, 5, &sub));   // module buffer too small
}

static void test_queue(void)
{
    BlRelayQueue q;
    bl_relay_queue_init(&q);
    BlRelayOrder o;
    CHECK(!bl_relay_queue_pop(&q, &o));
    // Wraps: fill, drain a few, fill again, order kept.
    for (int round = 0; round < 3; round++) {
        char sub[16], pay[8];
        for (int i = 0; i < BL_RELAY_QUEUE; i++) {
            snprintf(sub, sizeof(sub), "o/%d", i);
            snprintf(pay, sizeof(pay), "%d", i);
            CHECK(bl_relay_queue_push(&q, sub, pay, strlen(pay), i % 2));
        }
        CHECK(!bl_relay_queue_push(&q, "o/x", "x", 1, false));   // full: refused, none dropped
        for (int i = 0; i < BL_RELAY_QUEUE; i++) {
            snprintf(sub, sizeof(sub), "o/%d", i);
            if (!CHECK(bl_relay_queue_pop(&q, &o))) return;
            CHECK(!strcmp(o.sub, sub) && o.len == 1 && o.payload[0] == (char)('0' + i) && o.retained == (uint8_t)(i % 2));
        }
        CHECK(!bl_relay_queue_pop(&q, &o));
    }
    CHECK(q.refused == 3);
}

static void test_queue_refuses(void)
{
    BlRelayQueue q;
    bl_relay_queue_init(&q);
    char big[BL_RELAY_ORDER_MAX + 1];
    memset(big, 'x', sizeof(big));
    CHECK(!bl_relay_queue_push(&q, "o", big, sizeof(big), false));           // payload too long
    CHECK(bl_relay_queue_push(&q, "o", big, BL_RELAY_ORDER_MAX, false));      // exactly the limit
    CHECK(!bl_relay_queue_push(&q, "../o", "x", 1, false));                   // sub refused
    CHECK(!bl_relay_queue_push(&q, "o", NULL, 1, false));                     // bytes promised, none given
    CHECK(bl_relay_queue_push(&q, "o", NULL, 0, true));                       // an empty retained order (a clear)
    CHECK(q.count == 2 && q.refused == 3);
    BlRelayOrder o;
    CHECK(bl_relay_queue_pop(&q, &o) && o.len == BL_RELAY_ORDER_MAX);
    CHECK(bl_relay_queue_pop(&q, &o) && o.len == 0 && o.retained == 1);
}

static void test_codec(void)
{
    uint8_t buf[BL_RELAY_PAYLOAD_MAX + BL_RELAY_SUB_MAX + 8];
    // What a homebrew publishes is what the agent reads back (the same layout both ways).
    const size_t n = bl_relay_encode_publish("downloads/12", "{\"a\":1}", 7, true, buf, sizeof(buf));
    CHECK(n == 1 + 12 + 1 + 7 && buf[0] == 1 && !strcmp((char *)buf + 1, "downloads/12"));
    const char *sub = NULL;
    const void *payload = NULL;
    size_t plen = 0;
    bool retained = false;
    CHECK(bl_relay_decode_order(buf, n, &sub, &payload, &plen, &retained) && !strcmp(sub, "downloads/12") && plen == 7 &&
          !memcmp(payload, "{\"a\":1}", 7) && retained);
    // An empty payload, not retained.
    const size_t e = bl_relay_encode_publish("x", NULL, 0, false, buf, sizeof(buf));
    CHECK(e == 3 && bl_relay_decode_order(buf, e, &sub, &payload, &plen, &retained) && plen == 0 && !retained);

    // Refused on the way out: a topic that is not allowed, too large, no room.
    CHECK(!bl_relay_encode_publish("../x", "1", 1, false, buf, sizeof(buf)));
    CHECK(!bl_relay_encode_publish("x", NULL, 1, false, buf, sizeof(buf)));
    CHECK(!bl_relay_encode_publish("x", buf, BL_RELAY_PAYLOAD_MAX + 1, false, buf, sizeof(buf)));
    CHECK(!bl_relay_encode_publish("x", "12", 2, false, buf, 4) && bl_relay_encode_publish("x", "12", 2, false, buf, 5));
    CHECK(!bl_relay_encode_publish("x", "1", 1, false, NULL, 100));

    // Refused on the way in: too short, no NUL, a topic that climbs, a payload too long.
    const uint8_t no_nul[] = { 0, 's', 't', 'a', 't', 'e' };
    CHECK(!bl_relay_decode_order(no_nul, sizeof(no_nul), &sub, &payload, &plen, &retained));
    CHECK(!bl_relay_decode_order(no_nul, 2, &sub, &payload, &plen, &retained) && !bl_relay_decode_order(NULL, 9, &sub, &payload, &plen, &retained));
    const uint8_t up[] = { 0, '.', '.', '/', 'x', 0, 'p' };
    CHECK(!bl_relay_decode_order(up, sizeof(up), &sub, &payload, &plen, &retained));
    static uint8_t big[1 + 2 + BL_RELAY_ORDER_MAX + 1];
    memset(big, 'x', sizeof(big));
    big[0] = 0;
    big[2] = 0;
    CHECK(!bl_relay_decode_order(big, sizeof(big), &sub, &payload, &plen, &retained));          // 257 bytes of payload
    CHECK(bl_relay_decode_order(big, sizeof(big) - 1, &sub, &payload, &plen, &retained) && plen == BL_RELAY_ORDER_MAX);
    // The optional outputs may be left out.
    CHECK(bl_relay_decode_order(buf, e, NULL, NULL, NULL, NULL));
}

static void test_wire_sizes(void)
{
    CHECK(sizeof(BlRelayHello) == 64 && sizeof(BlRelayHelloReply) == 16 && sizeof(BlRelayStatus) == 8);
    CHECK(sizeof(((BlRelayHello *)0)->module) == BL_APP_MODULE_MAX);
}

// Off the console there is no agent: the client says so, and the homebrew keeps its own connection.
static void test_no_agent(void)
{
    BlRelayHelloReply rep;
    memset(&rep, 0xAA, sizeof(rep));
    uint32_t rc = 99;
    CHECK(!bl_relay_available() && !bl_relay_open("playshelf", "1.0.0", &rep, &rc) && rc == 0 && !rep.accepted);
    CHECK(!bl_relay_open("playshelf", "1.0.0", NULL, NULL) && !bl_relay_connected());
    CHECK(bl_relay_publish("state", "{}", 2, true) != 0);
    BlRelayInOrder in;
    bool got = true;
    CHECK(bl_relay_pop_order(&in, &got) != 0 && !got);
    BlRelayStatus st;
    CHECK(bl_relay_status(&st) != 0);
    bl_relay_close();   // nothing open: nothing happens
}

int main(void)
{
    test_modules();
    test_subs();
    test_topics();
    test_queue();
    test_queue_refuses();
    test_codec();
    test_wire_sizes();
    test_no_agent();
    CHECK_DONE("relay: ok");
}
