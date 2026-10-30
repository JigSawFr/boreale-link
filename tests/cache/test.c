// bl_cache: the icons and names shared by the homebrews, emptied when the
// console language changes or the icons are over their bounds, a newer
// schema left alone, the keys this version does not know kept.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bl_cache.h"
#include "check.h"

static char root[128];

static void path_of(const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", root, rel);
}

static void put(const char *rel, const char *text)
{
    char path[256], cmd[300];
    path_of(rel, path, sizeof(path));
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", path);
    REQUIRE(system(cmd) == 0);
    FILE *f = fopen(path, "wb");
    REQUIRE(f != NULL);
    fputs(text, f);
    fclose(f);
}

static size_t get(const char *rel, char *out, size_t cap)
{
    char path[256];
    path_of(rel, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return n;
}

static int exists(const char *rel)
{
    char path[256];
    struct stat st;
    path_of(rel, path, sizeof(path));
    return stat(path, &st) == 0;
}

static const unsigned char JPEG[] = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4 };

static void test_icons(void)
{
    BlCache c;
    char text[512];
    CHECK(!bl_cache_open(&c, root, 0) && !c.ready);    // the language unknown
    CHECK(!bl_cache_open(&c, "", 2) && !bl_cache_open(&c, NULL, 2));
    if (!CHECK(bl_cache_open(&c, root, 2) && c.ready)) return;
    CHECK(get("config/boreale/cache/cache.conf", text, sizeof(text)) && !strcmp(text, "schema=1\nlang=2\n"));

    char path[256];
    CHECK(bl_cache_icon_path("/", 0x0100000000010000ull, path, sizeof(path)) &&
          !strcmp(path, "/config/boreale/cache/icons/0100000000010000.jpg"));
    CHECK(bl_cache_icon_path("sdmc:/", 0x01000000000100ABull, path, sizeof(path)) &&
          !strcmp(path, "sdmc:/config/boreale/cache/icons/01000000000100AB.jpg"));
    CHECK(!bl_cache_icon_path("/", 1, path, 20));

    unsigned char buf[64];
    CHECK(bl_cache_icon_get(&c, 0x0100000000010000ull, buf, sizeof(buf)) == 0);   // none yet
    CHECK(bl_cache_icon_put(&c, 0x0100000000010000ull, JPEG, sizeof(JPEG)) && c.files == 1 && c.bytes == sizeof(JPEG));
    CHECK(bl_cache_icon_put(&c, 0x0100000000010000ull, JPEG, sizeof(JPEG)) && c.files == 1 && c.bytes == sizeof(JPEG));
    CHECK(!bl_cache_icon_put(&c, 0x0100000000020000ull, "not", 3));           // not a JPEG
    CHECK(bl_cache_icon_get(&c, 0x0100000000010000ull, buf, sizeof(buf)) == sizeof(JPEG) && !memcmp(buf, JPEG, sizeof(JPEG)));
    CHECK(bl_cache_icon_get(&c, 0x0100000000010000ull, buf, 4) == 0);          // does not fit
    CHECK(!exists("config/boreale/cache/icons/0100000000010000.tmp"));

    // Another homebrew, the same language: the icons counted, kept; a file
    // cut by a crash removed.
    put("config/boreale/cache/icons/0100000000030000.tmp", "cut");
    BlCache other;
    CHECK(bl_cache_open(&other, root, 2) && other.files == 1 && other.bytes == sizeof(JPEG));
    CHECK(!exists("config/boreale/cache/icons/0100000000030000.tmp"));
    CHECK(bl_cache_icon_get(&other, 0x0100000000010000ull, buf, sizeof(buf)) == sizeof(JPEG));

    // Another language: every icon and name gone.
    const uint64_t id = 0x0100000000010000ull;
    const char *name = "Star Kart Racers";
    CHECK(bl_cache_names_put(&other, &id, &name, 1));
    CHECK(bl_cache_open(&c, root, 3) && c.files == 0);
    CHECK(!exists("config/boreale/cache/icons/0100000000010000.jpg") && !exists("config/boreale/cache/names.txt"));
    CHECK(get("config/boreale/cache/cache.conf", text, sizeof(text)) && !strcmp(text, "schema=1\nlang=3\n"));
}

static void test_names(void)
{
    BlCache c;
    char name[BL_CACHE_NAME_MAX], text[1024];
    if (!CHECK(bl_cache_open(&c, root, 3))) return;
    CHECK(!bl_cache_name_get(&c, 1, name, sizeof(name)));   // no file yet
    const uint64_t ids[] = { 0x0100000000010000ull, 0x0100000000020000ull, 0x0100000000030000ull, 0x0100000000010000ull };
    const char *names[] = { "Old name", "Moonlit Garden", "Bad\nname", "Star Kart Racers" };
    CHECK(bl_cache_names_put(&c, ids, names, 4));
    CHECK(get("config/boreale/cache/names.txt", text, sizeof(text)) &&
          !strcmp(text, "schema=1\n0100000000020000=Moonlit Garden\n0100000000010000=Star Kart Racers\n"));
    CHECK(bl_cache_name_get(&c, ids[0], name, sizeof(name)) && !strcmp(name, "Star Kart Racers"));
    CHECK(!bl_cache_name_get(&c, ids[2], name, sizeof(name)));   // left out
    CHECK(!bl_cache_name_get(&c, ids[1], name, 4));              // does not fit

    // Another one later: the others kept, a line it does not know kept too.
    put("config/boreale/cache/names.txt", "schema=1\n0100000000020000=Moonlit Garden\nfuture=1\n0100000000010000=Star Kart Racers\n");
    const uint64_t one = 0x0100000000010000ull;
    const char *renamed = "Star Kart Racers Deluxe";
    CHECK(bl_cache_names_put(&c, &one, &renamed, 1));
    CHECK(get("config/boreale/cache/names.txt", text, sizeof(text)) &&
          !strcmp(text, "schema=1\n0100000000020000=Moonlit Garden\nfuture=1\n0100000000010000=Star Kart Racers Deluxe\n"));

    // A newer version's file: not read, never written over.
    put("config/boreale/cache/names.txt", "schema=2\n0100000000020000=Moonlit Garden\n");
    CHECK(!bl_cache_name_get(&c, ids[1], name, sizeof(name)));
    CHECK(!bl_cache_names_put(&c, &one, &renamed, 1));
    CHECK(get("config/boreale/cache/names.txt", text, sizeof(text)) && !strcmp(text, "schema=2\n0100000000020000=Moonlit Garden\n"));
}

static void test_conf(void)
{
    BlCache c;
    char text[512];
    // A key this version does not know: kept when it writes the file again.
    put("config/boreale/cache/cache.conf", "schema=1\nlang=3\nfuture=yes\n");
    CHECK(bl_cache_open(&c, root, 4));
    CHECK(get("config/boreale/cache/cache.conf", text, sizeof(text)) && !strcmp(text, "schema=1\nlang=4\nfuture=yes\n"));
    // The same language: not written at all.
    put("config/boreale/cache/cache.conf", "schema=1\nlang=4\n# a comment\n");
    CHECK(bl_cache_open(&c, root, 4));
    CHECK(get("config/boreale/cache/cache.conf", text, sizeof(text)) && !strcmp(text, "schema=1\nlang=4\n# a comment\n"));
    // A newer version's: left alone, the cache not used, its icons kept.
    put("config/boreale/cache/cache.conf", "schema=2\nlang=9\n");
    put("config/boreale/cache/icons/0100000000050000.jpg", "\xFF\xD8xx");
    CHECK(!bl_cache_open(&c, root, 4) && !c.ready);
    CHECK(!bl_cache_icon_put(&c, 1, JPEG, sizeof(JPEG)));
    CHECK(exists("config/boreale/cache/icons/0100000000050000.jpg"));
    CHECK(get("config/boreale/cache/cache.conf", text, sizeof(text)) && !strcmp(text, "schema=2\nlang=9\n"));
    // Without a schema line: the first one, read and written.
    put("config/boreale/cache/cache.conf", "lang=4\n");
    CHECK(bl_cache_open(&c, root, 4) && c.files == 1);
}

static void test_bounds(void)
{
    BlCache c;
    if (!CHECK(bl_cache_open(&c, root, 5))) return;
    // Over its bounds when opened: emptied.
    c.files = BL_CACHE_ICONS_MAX_FILES;
    CHECK(!bl_cache_icon_put(&c, 1, JPEG, sizeof(JPEG)));
    c.files = 0;
    c.bytes = BL_CACHE_ICONS_MAX_BYTES - 2;
    CHECK(!bl_cache_icon_put(&c, 1, JPEG, sizeof(JPEG)));
    c.bytes = 0;
    CHECK(bl_cache_icon_put(&c, 1, JPEG, sizeof(JPEG)));
    // names.txt grown past its bound: started again with the icons.
    char path[256];
    path_of("config/boreale/cache/names.txt", path, sizeof(path));
    FILE *f = fopen(path, "wb");
    REQUIRE(f != NULL);
    fputs("schema=1\n", f);
    for (int i = 0; i < 40000; i++) fprintf(f, "%016X=Some made-up game\n", i);
    fclose(f);
    CHECK(bl_cache_open(&c, root, 5) && c.files == 0 && !exists("config/boreale/cache/names.txt"));
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/bl_cache_XXXXXX");
    REQUIRE(mkdtemp(root) != NULL);
    test_icons();
    test_names();
    test_conf();
    test_bounds();
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
    return CHECK_DONE("bl_cache: icons, names, language, bounds and schema assertions passed");
}
