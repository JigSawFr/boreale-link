// bl_agent_conf: both checks off by default, read and written back with the
// keys a version does not know.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <string.h>

#include "bl_agent_conf.h"
#include "check.h"

static void test_defaults(void)
{
    BlAgentConf c;
    bl_agent_conf_defaults(&c);
    CHECK(!c.check_versions && !c.check_clock && !strcmp(c.ntp_server, BL_AGENT_NTP_DEFAULT));
    bl_agent_conf_parse(&c, "", 0);   // an empty file: the same
    CHECK(!c.check_versions && !c.check_clock);
}

static void test_round_trip(void)
{
    BlAgentConf c;
    const char *text = "schema=1\ncheck_versions=on\ncheck_clock=0\nntp_server=europe.pool.ntp.org\nfuture=1\n";
    bl_agent_conf_parse(&c, text, strlen(text));
    CHECK(c.check_versions && !c.check_clock && !strcmp(c.ntp_server, "europe.pool.ntp.org"));
    char out[512];
    const size_t n = bl_agent_conf_write(&c, out, sizeof(out));
    CHECK(n && strstr(out, "check_versions=1\ncheck_clock=0\nntp_server=europe.pool.ntp.org\n") && strstr(out, "future=1\n"));
    CHECK(bl_agent_conf_write(&c, out, 20) == 0);

    BlAgentConf d;
    bl_agent_conf_parse(&d, out, n);
    CHECK(d.check_versions && !d.check_clock);
}

static void test_invalid_and_newer(void)
{
    BlAgentConf c;
    const char *text = "check_versions=maybe\nntp_server=bad host!\n";
    bl_agent_conf_parse(&c, text, strlen(text));
    CHECK(!c.check_versions && !strcmp(c.ntp_server, BL_AGENT_NTP_DEFAULT));   // not valid: the default stays
    CHECK(bl_agent_ntp_valid("0.fr.pool.ntp.org") && !bl_agent_ntp_valid("") && !bl_agent_ntp_valid("a b"));
    CHECK(!bl_agent_ntp_valid("a\nb=1"));

    const char *newer = "schema=3\ncheck_clock=1\n";
    bl_agent_conf_parse(&c, newer, strlen(newer));
    CHECK(c.check_clock && !bl_agent_conf_may_write(&c));
}

int main(void)
{
    test_defaults();
    test_round_trip();
    test_invalid_and_newer();
    return CHECK_DONE("agent_conf: assertions passed");
}
