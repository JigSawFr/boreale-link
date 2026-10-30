// bl_app: a homebrew's file in the registry, its name and path, read and
// written back with the keys a version does not know.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>

#include "bl_app.h"
#include "check.h"

static int contains(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

static void test_names(void)
{
    char out[96];
    CHECK(bl_app_path("playguard", out, sizeof(out)) && !strcmp(out, "config/boreale/apps/playguard.conf"));
    CHECK(!bl_app_path("PlayGuard", out, sizeof(out)) && !bl_app_path("a/b", out, sizeof(out)));
    CHECK(!bl_app_path("", out, sizeof(out)) && !bl_app_path(NULL, out, sizeof(out)));
    CHECK(!bl_app_path("playguard", out, 20));   // too small
    CHECK(bl_app_module_valid("play_shelf-2") && !bl_app_module_valid("a b") && !bl_app_module_valid(".."));
    char long_module[BL_APP_MODULE_MAX + 1];
    memset(long_module, 'a', BL_APP_MODULE_MAX);
    long_module[BL_APP_MODULE_MAX] = '\0';
    CHECK(!bl_app_module_valid(long_module));

    char module[BL_APP_MODULE_MAX];
    CHECK(bl_app_module_of("playshelf.conf", module, sizeof(module)) && !strcmp(module, "playshelf"));
    CHECK(!bl_app_module_of(".conf", module, sizeof(module)) && !bl_app_module_of("playshelf.jpg", module, sizeof(module)));
    CHECK(!bl_app_module_of("PlayShelf.conf", module, sizeof(module)) && !bl_app_module_of(NULL, module, sizeof(module)));
    CHECK(!bl_app_module_of("playshelf.conf.tmp", module, sizeof(module)));
    CHECK(!bl_app_module_of("playshelf.conf", module, 4));   // too small
}

static void test_round_trip(void)
{
    BlApp a;
    bl_app_defaults(&a);
    CHECK(!a.read && a.file.schema == BL_APP_SCHEMA && bl_app_may_write(&a));
    snprintf(a.name, sizeof(a.name), "PlayGuard");
    snprintf(a.version, sizeof(a.version), "1.4.0");
    snprintf(a.nro, sizeof(a.nro), "switch/PlayGuard/PlayGuard.nro");
    snprintf(a.repo, sizeof(a.repo), "JigSawFr/PlayGuard");
    char out[1024];
    size_t n = bl_app_write(&a, out, sizeof(out));
    CHECK(n > 0 && contains(out, "schema=1\n") && contains(out, "name=PlayGuard\n") && contains(out, "icon=\n"));
    char small[40];
    CHECK(bl_app_write(&a, small, sizeof(small)) == 0);   // too small

    // A newer version's key kept, and written back after its own.
    char text[1100];
    snprintf(text, sizeof(text), "%sfuture_key=42\n", out);
    BlApp b;
    bl_app_defaults(&b);
    bl_app_parse(&b, text, strlen(text));
    CHECK(b.read && !strcmp(b.name, "PlayGuard") && !strcmp(b.version, "1.4.0") && !b.icon[0]);
    CHECK(!strcmp(b.nro, "switch/PlayGuard/PlayGuard.nro") && !strcmp(b.repo, "JigSawFr/PlayGuard"));
    CHECK(bl_app_may_write(&b));
    snprintf(b.version, sizeof(b.version), "1.5.0");
    BlApp copy = b;   // a copy keeps its kept lines
    n = bl_app_write(&copy, out, sizeof(out));
    CHECK(n > 0 && contains(out, "version=1.5.0\n") && contains(out, "future_key=42\n"));

    // Written again with nothing changed: the same bytes (no write needed).
    BlApp c;
    bl_app_defaults(&c);
    bl_app_parse(&c, out, n);
    char again[1024];
    CHECK(bl_app_write(&c, again, sizeof(again)) == n && !memcmp(again, out, n));
}

static void test_bad_values(void)
{
    // A value too long: what was there stays.
    char text[400] = "name=PlayShelf\nname=";
    memset(text + strlen(text), 'x', 100);
    strcat(text, "\n");
    BlApp a;
    bl_app_defaults(&a);
    bl_app_parse(&a, text, strlen(text));
    CHECK(!strcmp(a.name, "PlayShelf") && bl_app_may_write(&a));
}

static void test_newer_schema(void)
{
    const char text[] = "schema=2\nname=PlayShelf\nversion=2.0.0\n";
    BlApp a;
    bl_app_defaults(&a);
    bl_app_parse(&a, text, strlen(text));
    CHECK(!strcmp(a.name, "PlayShelf") && a.file.schema == 2 && !bl_app_may_write(&a));
    // No schema line: schema 1.
    const char old[] = "name=PlayShelf\n";
    bl_app_defaults(&a);
    bl_app_parse(&a, old, strlen(old));
    CHECK(a.file.schema == 1 && bl_app_may_write(&a));
    // An empty file: nothing read, written as new.
    bl_app_defaults(&a);
    bl_app_parse(&a, "", 0);
    CHECK(!a.read && bl_app_may_write(&a));
}

int main(void)
{
    test_names();
    test_round_trip();
    test_bad_values();
    test_newer_schema();
    return CHECK_DONE("app: all tests passed");
}
