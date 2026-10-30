// bl_link_conf: the shared broker settings, read, checked and written back
// with the keys a version does not know.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>

#include "bl_link_conf.h"
#include "check.h"

static int contains(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

static void test_defaults(void)
{
    BlLinkConf c;
    bl_link_conf_defaults(&c);
    CHECK(!c.enabled && !c.host[0] && c.port == 1883 && !c.tls && c.mqtt_version == BlLinkMqtt_Auto);
    CHECK(!strcmp(c.topic_prefix, "boreale") && !strcmp(c.discovery_prefix, "homeassistant"));
    CHECK(!c.read && !c.console_id[0] && bl_link_conf_may_write(&c));
    char out[2048];
    const size_t n = bl_link_conf_write(&c, out, sizeof(out));
    CHECK(n > 0 && contains(out, "schema=1\n") && contains(out, "port=1883\n") && contains(out, "mqtt_version=auto\n"));
    CHECK(bl_link_conf_write(&c, out, 40) == 0);   // too small
}

static void test_round_trip(void)
{
    const char text[] =
        "schema=1\nenabled=1\nhost=mqtt.lan\nport=8883\ntls=yes\nca_file=config/boreale/ca.pem\n"
        "mqtt_version=3.1.1\nusername=console\npassword=p=ss word#1\nallow_anonymous=0\n"
        "console_id=5a0c3e11\nconsole_name=Salon\ntopic_prefix=home_switch\ndiscovery_prefix=ha\n"
        "future_key=42\n";
    BlLinkConf c;
    bl_link_conf_defaults(&c);
    bl_link_conf_parse(&c, text, strlen(text));
    CHECK(c.read && c.enabled && !strcmp(c.host, "mqtt.lan") && c.port == 8883 && c.tls);
    CHECK(!strcmp(c.ca_file, "config/boreale/ca.pem") && c.mqtt_version == BlLinkMqtt_V311);
    CHECK(!strcmp(c.username, "console") && !strcmp(c.password, "p=ss word#1") && !c.allow_anonymous);
    CHECK(!strcmp(c.console_id, "5a0c3e11") && !strcmp(c.console_name, "Salon"));
    CHECK(!strcmp(c.topic_prefix, "home_switch") && !strcmp(c.discovery_prefix, "ha"));
    CHECK(bl_link_conf_may_write(&c));
    // Written back: its own keys, then the one it does not know, as read.
    BlLinkConf copy = c;   // a copy keeps its kept lines
    char out[2048];
    CHECK(bl_link_conf_write(&copy, out, sizeof(out)) > 0);
    CHECK(contains(out, "password=p=ss word#1\n") && contains(out, "future_key=42\n"));
    BlLinkConf again;
    bl_link_conf_defaults(&again);
    bl_link_conf_parse(&again, out, strlen(out));
    CHECK(!strcmp(again.host, "mqtt.lan") && again.port == 8883 && !strcmp(again.console_id, "5a0c3e11"));
    CHECK(contains(again.kept, "future_key=42\n"));
}

static void test_bad_values(void)
{
    // Out of range or malformed: the default stays.
    const char text[] = "port=0\nport=70000\ntls=maybe\nconsole_id=5A0C3E11\nconsole_id=123\n"
                        "topic_prefix=a/b\ndiscovery_prefix=\nmqtt_version=6\n";
    BlLinkConf c;
    bl_link_conf_defaults(&c);
    bl_link_conf_parse(&c, text, strlen(text));
    CHECK(c.port == 1883 && !c.tls && !c.console_id[0] && c.mqtt_version == BlLinkMqtt_Auto);
    CHECK(!strcmp(c.topic_prefix, "boreale") && !strcmp(c.discovery_prefix, "homeassistant"));
    CHECK(bl_link_id_valid("5a0c3e11") && !bl_link_id_valid("5a0c3e1g") && !bl_link_id_valid(NULL));
    CHECK(bl_link_prefix_valid("boreale-2_x") && !bl_link_prefix_valid("a b") && !bl_link_prefix_valid(""));
}

static void test_newer_schema(void)
{
    // A newer version's file: read for what is known, never written over.
    const char text[] = "schema=2\nhost=broker.lan\nenabled=1\n";
    BlLinkConf c;
    bl_link_conf_defaults(&c);
    bl_link_conf_parse(&c, text, strlen(text));
    CHECK(c.enabled && !strcmp(c.host, "broker.lan") && c.file.schema == 2 && !bl_link_conf_may_write(&c));
    // A line too long to read: not written over either (it would be lost).
    char longer[700] = "host=a\nnote=";
    memset(longer + strlen(longer), 'x', 600);
    longer[sizeof(longer) - 1] = '\0';
    bl_link_conf_defaults(&c);
    bl_link_conf_parse(&c, longer, strlen(longer));
    CHECK(!strcmp(c.host, "a") && c.file.skipped == 1 && !bl_link_conf_may_write(&c));
    // No schema line: a file written before it, schema 1.
    const char old[] = "host=old.lan\n";
    bl_link_conf_defaults(&c);
    bl_link_conf_parse(&c, old, strlen(old));
    CHECK(c.file.schema == 1 && bl_link_conf_may_write(&c));
}

int main(void)
{
    test_defaults();
    test_round_trip();
    test_bad_values();
    test_newer_schema();
    return CHECK_DONE("link_conf: all tests passed");
}
