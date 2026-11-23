// bl_latest: the file read and written back, a reading due, versions out of a
// tag, a redirect and switchbrew's table.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <string.h>

#include "bl_latest.h"
#include "check.h"

static void test_round_trip(void)
{
    BlLatest l;
    bl_latest_defaults(&l);
    CHECK(l.checked == 0 && l.clock_checked == 0 && !l.atmosphere[0]);   // absent: unknown
    CHECK(bl_latest_versions_due(&l, 1000) && bl_latest_clock_due(&l, 1000));
    snprintf(l.atmosphere, sizeof(l.atmosphere), "1.12.0");
    snprintf(l.syspatch, sizeof(l.syspatch), "1.6.3.0");
    snprintf(l.clock_server, sizeof(l.clock_server), "pool.ntp.org");
    l.checked = 1792000000;
    l.clock_checked = 1792000100;
    l.clock_network = -2;
    l.clock_user = 40;
    char out[1024];
    const size_t n = bl_latest_write(&l, out, sizeof(out));
    CHECK(n && strstr(out, "schema=1\nchecked=1792000000\natmosphere=1.12.0\n"));
    CHECK(bl_latest_write(&l, out, 40) == 0);   // too small

    BlLatest r;
    bl_latest_parse(&r, out, n);
    CHECK(r.checked == 1792000000 && r.clock_checked == 1792000100);
    CHECK(!strcmp(r.atmosphere, "1.12.0") && !strcmp(r.syspatch, "1.6.3.0") && !r.hekate[0]);
    CHECK(r.clock_network == -2 && r.clock_user == 40 && !strcmp(r.clock_server, "pool.ntp.org"));
    CHECK(bl_latest_may_write(&r));
}

static void test_unknown_and_newer(void)
{
    BlLatest l;
    const char *text = "schema=1\nhekate=6.5.4\nfuture_key=kept\nfirmware=garbage\nclock_network=abc\nclock_user=99999999999\n";
    bl_latest_parse(&l, text, strlen(text));
    CHECK(!strcmp(l.hekate, "6.5.4") && !l.firmware[0]);   // not a version: unknown
    CHECK(l.clock_network == 0 && l.clock_user == 0);      // not a number, out of range
    char out[512];
    const size_t n = bl_latest_write(&l, out, sizeof(out));
    CHECK(n && strstr(out, "future_key=kept\n"));          // written back

    const char *newer = "schema=2\nhekate=6.5.4\n";
    bl_latest_parse(&l, newer, strlen(newer));
    CHECK(!strcmp(l.hekate, "6.5.4") && !bl_latest_may_write(&l));   // read, never written over
}

static void test_due(void)
{
    BlLatest l;
    bl_latest_defaults(&l);
    l.checked = 100000;
    l.clock_checked = 100000;
    CHECK(!bl_latest_versions_due(&l, 100000 + 3600) && !bl_latest_clock_due(&l, 100000 + 3600));
    CHECK(!bl_latest_versions_due(&l, 100000 + BL_LATEST_VERSIONS_EVERY_S - 1));
    CHECK(bl_latest_versions_due(&l, 100000 + BL_LATEST_VERSIONS_EVERY_S));
    CHECK(bl_latest_clock_due(&l, 100000 + BL_LATEST_CLOCK_EVERY_S));
    CHECK(bl_latest_versions_due(&l, 50000));   // the clock set back
}

static void test_tags(void)
{
    char v[32];
    CHECK(bl_version_of_tag("v6.5.4", v, sizeof(v)) && !strcmp(v, "6.5.4"));
    CHECK(bl_version_of_tag("1.12.0", v, sizeof(v)) && !strcmp(v, "1.12.0"));
    CHECK(bl_version_of_tag("v1.6.3.0", v, sizeof(v)) && !strcmp(v, "1.6.3.0"));
    CHECK(!bl_version_of_tag("6", v, sizeof(v)) && !bl_version_of_tag("", v, sizeof(v)));
    CHECK(!bl_version_of_tag("1.2.3.4.5", v, sizeof(v)) && !bl_version_of_tag("1.2.", v, sizeof(v)));
    CHECK(!bl_version_of_tag("v1.2.3-rc1", v, sizeof(v)) && !bl_version_of_tag(NULL, v, sizeof(v)));
    CHECK(!bl_version_of_tag("1.2.3", v, 3));   // too small

    CHECK(bl_tag_of_url("https://github.com/CTCaer/hekate/releases/tag/v6.5.4", v, sizeof(v)) && !strcmp(v, "v6.5.4"));
    CHECK(bl_tag_of_url("/o/r/releases/tag/1.12.0?x=1", v, sizeof(v)) && !strcmp(v, "1.12.0"));
    CHECK(!bl_tag_of_url("https://github.com/o/r/releases", v, sizeof(v)) && !bl_tag_of_url("/releases/tag/", v, sizeof(v)));
    CHECK(!bl_tag_of_url(NULL, v, sizeof(v)));

    CHECK(bl_version_compare("1.12.0", "1.9.9") > 0 && bl_version_compare("1.2", "1.2.0") == 0);
    CHECK(bl_version_compare("1.6.3.0", "1.6.3.1") < 0 && bl_version_compare("23.0.1", "23.0.1") == 0);
}

static const char *TABLE =
    "{| class=\"wikitable\"\n! Version !! Date\n|-\n| [[22.1.0]]\n| 2026-01-01\n|-\n"
    "| [[23.0.1]]\n| 2026-05-01\n|-\n| [[19.0.2]] (CHN-only)\n| x\n|-\n| [[23.0.0|23.0.0]]\n"
    "| [[24.0]] not three numbers\n   | [[9.9.9]]\n| text [[30.0.0]] not at the start\n|}\n";

static void test_firmware(void)
{
    BlFirmwareScan s;
    bl_firmware_scan_init(&s);
    bl_firmware_scan_feed(&s, TABLE, strlen(TABLE));
    CHECK(!strcmp(bl_firmware_scan_end(&s), "23.0.1"));

    // The same text in pieces of 7 bytes, rows cut anywhere, and no final newline.
    const size_t len = strlen(TABLE) - 1;
    bl_firmware_scan_init(&s);
    for (size_t i = 0; i < len; i += 7) bl_firmware_scan_feed(&s, TABLE + i, i + 7 <= len ? 7 : len - i);
    CHECK(!strcmp(bl_firmware_scan_end(&s), "23.0.1"));

    bl_firmware_scan_init(&s);
    bl_firmware_scan_feed(&s, "nothing here\n| 1.2.3\n", 21);
    CHECK(!strcmp(bl_firmware_scan_end(&s), ""));

    // A row that is the last line, without a newline; a very long line is ignored.
    bl_firmware_scan_init(&s);
    char longline[300];
    memset(longline, 'x', sizeof(longline));
    memcpy(longline, "| [[99.0.0]] ", 13);
    bl_firmware_scan_feed(&s, longline, sizeof(longline));
    bl_firmware_scan_feed(&s, "\n| [[5.1.0]]", 12);
    CHECK(!strcmp(bl_firmware_scan_end(&s), "5.1.0"));
}

int main(void)
{
    test_round_trip();
    test_unknown_and_newer();
    test_due();
    test_tags();
    test_firmware();
    return CHECK_DONE("latest: assertions passed");
}
