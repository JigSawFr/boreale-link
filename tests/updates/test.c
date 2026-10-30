// bl_updates: the newest release of each homebrew, read and written back
// with the keys a version does not know, a check due once a day.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <string.h>

#include "bl_updates.h"
#include "check.h"

static void test_round_trip(void)
{
    BlUpdates u;
    bl_updates_defaults(&u);
    CHECK(bl_updates_get(&u, "playguard") == NULL && bl_updates_due(&u, 1000));   // never checked
    CHECK(bl_updates_set(&u, "playguard", "1.4.0") && bl_updates_set(&u, "playshelf", "0.9.2"));
    CHECK(bl_updates_set(&u, "playguard", "1.4.1") && u.count == 2);            // replaced
    CHECK(!bl_updates_set(&u, "Play Guard", "1.0") && !bl_updates_set(&u, "lantern", ""));
    CHECK(!bl_updates_set(&u, "lantern", "1 0") && !bl_updates_set(&u, NULL, "1.0"));
    u.checked = 1792000000;
    char out[1024];
    const size_t n = bl_updates_write(&u, out, sizeof(out));
    CHECK(n && strstr(out, "schema=1\nchecked=1792000000\nlatest.playguard=1.4.1\nlatest.playshelf=0.9.2\n"));
    CHECK(bl_updates_write(&u, out, 30) == 0);   // too small

    BlUpdates v;
    bl_updates_defaults(&v);
    bl_updates_parse(&v, out, n);
    CHECK(v.checked == 1792000000 && !strcmp(bl_updates_get(&v, "playshelf"), "0.9.2"));
    CHECK(bl_updates_may_write(&v));
}

static void test_due(void)
{
    BlUpdates u;
    bl_updates_defaults(&u);
    u.checked = 100000;
    CHECK(!bl_updates_due(&u, 100000 + 3600));
    CHECK(bl_updates_due(&u, 100000 + BL_UPDATES_EVERY_S));
    CHECK(bl_updates_due(&u, 50000));   // the clock set back
}

static void test_kept_and_schema(void)
{
    const char *text = "schema=1\nchecked=nope\nlatest.playguard=1.2.0\nfuture=yes\nlatest.Bad=1\nlatest.lantern=\n";
    BlUpdates u;
    bl_updates_defaults(&u);
    bl_updates_parse(&u, text, strlen(text));
    CHECK(u.checked == 0 && u.count == 1 && !strcmp(bl_updates_get(&u, "playguard"), "1.2.0"));
    char out[1024];
    CHECK(bl_updates_write(&u, out, sizeof(out)) && strstr(out, "future=yes\n") && !strstr(out, "Bad"));
    BlUpdates copy = u;   // a copy keeps its own lines
    char out2[1024];
    CHECK(bl_updates_write(&copy, out2, sizeof(out2)) && !strcmp(out, out2));

    // Past the bound: the extra homebrews kept as read, not lost.
    char many[2048] = "schema=1\n";
    for (int i = 0; i < BL_UPDATES_MAX + 2; i++) {
        char line[40];
        snprintf(line, sizeof(line), "latest.app%d=1.%d\n", i, i);
        strcat(many, line);
    }
    bl_updates_defaults(&u);
    bl_updates_parse(&u, many, strlen(many));
    CHECK(u.count == BL_UPDATES_MAX && !bl_updates_set(&u, "another", "1.0"));
    CHECK(bl_updates_write(&u, out, sizeof(out)) && strstr(out, "latest.app17=1.17\n"));

    const char *newer = "schema=2\nlatest.playguard=9.0\n";
    bl_updates_defaults(&u);
    bl_updates_parse(&u, newer, strlen(newer));
    CHECK(!bl_updates_may_write(&u) && !strcmp(bl_updates_get(&u, "playguard"), "9.0"));
}

int main(void)
{
    test_round_trip();
    test_due();
    test_kept_and_schema();
    return CHECK_DONE("bl_updates: round trip, due, kept keys and schema assertions passed");
}
