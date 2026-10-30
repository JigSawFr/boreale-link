// bl_jread and bl_notices: the JSON reader, and the shared bell's files read,
// checked and written back with what a version does not know.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>

#include "bl_notices.h"
#include "check.h"

static BlJTok toks[BL_NOTICES_TOKENS];

static int contains(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

static int parse(const char *text)
{
    return bl_jread_parse(text, strlen(text), toks, BL_NOTICES_TOKENS);
}

static void test_reader(void)
{
    const char doc[] = " {\"a\": [1, -2.5e3, true, null], \"b\": {\"c\": \"x\\\"y\"}, \"d\": false} ";
    const int n = parse(doc);
    // {, "a", [, 1, -2.5e3, true, null, "b", {, "c", "x\"y", "d", false
    CHECK(n == 13 && toks[0].type == BlJ_Obj && toks[0].size == 3);
    CHECK(bl_jread_eq(doc, &toks[1], "a") && toks[1].size == 1 && toks[2].type == BlJ_Arr && toks[2].size == 4);
    CHECK(bl_jread_skip(toks, n, 2) == 7 && bl_jread_eq(doc, &toks[7], "b"));
    CHECK(bl_jread_skip(toks, n, 8) == 11 && bl_jread_eq(doc, &toks[11], "d") && bl_jread_skip(toks, n, 12) == n);
    int64_t v = 0;
    bool b = true;
    CHECK(bl_jread_int(doc, &toks[3], &v) && v == 1 && !bl_jread_int(doc, &toks[4], &v));
    CHECK(bl_jread_bool(doc, &toks[5], &b) && b && !bl_jread_bool(doc, &toks[6], &b));
    CHECK(bl_jread_bool(doc, &toks[12], &b) && !b);
    char s[16];
    CHECK(bl_jread_str(doc, &toks[10], s, sizeof(s)) && !strcmp(s, "x\"y"));
    char member[64];
    CHECK(bl_jread_member(doc, toks, n, 7, member, sizeof(member)) && !strcmp(member, "\"b\":{\"c\": \"x\\\"y\"}"));
    CHECK(bl_jread_member(doc, toks, n, 1, member, 8) == 0);   // does not fit

    // Not JSON, or not whole.
    const char *bad[] = {"", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1,]", "[1 2]", "{1:2}", "{\"a\":1}}",
                         "{\"a\":tru}", "{\"a\":01}", "{\"a\":\"x}", "{\"a\":1} x", "{\"a\" 1}", "[\"a\":1]",
                         "{,}", "{\"a\":1,,\"b\":2}", "\"\x01\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (!CHECK(parse(bad[i]) < 0)) fprintf(stderr, "  accepted: %s\n", bad[i]);
    CHECK(parse("[]") == 1 && parse("{}") == 1 && parse("[[],{}]") == 3 && parse("7") == 1);
    BlJTok few[2];
    CHECK(bl_jread_parse("[1,2,3]", 7, few, 2) == -2);   // not enough tokens
}

static void test_strings(void)
{
    const char doc[] = "[\"caf\\u00e9 \\ud83c\\udfae\", \"\\n\\t\\/\", \"\\ud83c\", \"\\x\", \"Pokéa\", 12]";
    const int n = parse(doc);
    CHECK(n == 7);
    char s[32];
    CHECK(bl_jread_str(doc, &toks[1], s, sizeof(s)) && !strcmp(s, "café \xF0\x9F\x8E\xAE"));
    CHECK(bl_jread_str(doc, &toks[2], s, sizeof(s)) && !strcmp(s, "\n\t/"));
    CHECK(!bl_jread_str(doc, &toks[3], s, sizeof(s)) && !bl_jread_str(doc, &toks[4], s, sizeof(s)));
    CHECK(!bl_jread_str(doc, &toks[6], s, sizeof(s)));   // not a string
    // Cut at a character's boundary: "Poké" is 5 bytes, 'é' two.
    char small[5];
    CHECK(bl_jread_str(doc, &toks[5], small, sizeof(small)) && !strcmp(small, "Pok"));
    char tiny[4];
    CHECK(bl_jread_str(doc, &toks[1], tiny, sizeof(tiny)) && !strcmp(tiny, "caf"));
    // Integers: their bounds.
    const char nums[] = "[9223372036854775807, -9223372036854775808, 9223372036854775808, 1.0, -0]";
    CHECK(parse(nums) == 6);
    int64_t v = 0;
    CHECK(bl_jread_int(nums, &toks[1], &v) && v == INT64_MAX);
    CHECK(bl_jread_int(nums, &toks[2], &v) && v == INT64_MIN);
    CHECK(!bl_jread_int(nums, &toks[3], &v) && !bl_jread_int(nums, &toks[4], &v));
    CHECK(bl_jread_int(nums, &toks[5], &v) && v == 0);
}

static void test_names(void)
{
    char out[96];
    CHECK(bl_notices_path("playshelf", out, sizeof(out)) && !strcmp(out, "config/boreale/notices/playshelf.json"));
    CHECK(!bl_notices_path("Play", out, sizeof(out)) && !bl_notices_path("playshelf", out, 10));
    char module[BL_APP_MODULE_MAX];
    CHECK(bl_notices_module_of("playguard.json", module, sizeof(module)) && !strcmp(module, "playguard"));
    CHECK(!bl_notices_module_of("playguard.conf", module, sizeof(module)) && !bl_notices_module_of(".json", module, sizeof(module)));
    CHECK(!bl_notices_module_of("a.json.tmp", module, sizeof(module)));

    CHECK(bl_route_valid("") && bl_route_valid("downloads/queue") && bl_route_valid("console/remote-2"));
    CHECK(!bl_route_valid("/downloads") && !bl_route_valid("downloads/") && !bl_route_valid("a//b"));
    CHECK(!bl_route_valid("Downloads") && !bl_route_valid("a b") && !bl_route_valid("a;b") && !bl_route_valid(NULL));

    char path[64], args[128];
    CHECK(bl_open_args("switch/PlayShelf/PlayShelf.nro", "downloads/queue", path, sizeof(path), args, sizeof(args)));
    CHECK(!strcmp(path, "sdmc:/switch/PlayShelf/PlayShelf.nro"));
    CHECK(!strcmp(args, "sdmc:/switch/PlayShelf/PlayShelf.nro --open=downloads/queue"));
    CHECK(bl_open_args("/switch/My Apps/x.nro", "", path, sizeof(path), args, sizeof(args)));
    CHECK(!strcmp(path, "sdmc:/switch/My Apps/x.nro") && !strcmp(args, "\"sdmc:/switch/My Apps/x.nro\""));
    CHECK(!bl_open_args("switch/x.nro", "a b", path, sizeof(path), args, sizeof(args)));
    CHECK(!bl_open_args("", "", path, sizeof(path), args, sizeof(args)));
    CHECK(!bl_open_args("switch/x.nro", "downloads", path, sizeof(path), args, 20));   // does not fit

    char route[BL_NOTICE_ROUTE_MAX];
    char *argv1[] = {"sdmc:/switch/x.nro", "--open=console/remote", NULL};
    CHECK(bl_open_route(2, argv1, route, sizeof(route)) && !strcmp(route, "console/remote"));
    char *argv2[] = {"sdmc:/switch/x.nro", "--open=../etc", NULL};
    CHECK(!bl_open_route(2, argv2, route, sizeof(route)));
    char *argv3[] = {"sdmc:/switch/x.nro", NULL};
    CHECK(!bl_open_route(1, argv3, route, sizeof(route)));
}

static BlNotice notice(const char *id, BlNoticeLevel level, const char *title)
{
    BlNotice n;
    memset(&n, 0, sizeof(n));
    snprintf(n.id, sizeof(n.id), "%s", id);
    n.level = level;
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.lang, sizeof(n.lang), "en");
    n.progress = -1;
    n.updated = 1792000000;
    return n;
}

static void test_round_trip(void)
{
    static BlNotices f, g;
    bl_notices_init(&f, "playshelf", "switch/PlayShelf/PlayShelf.nro");
    CHECK(f.schema == BL_NOTICES_SCHEMA && bl_notices_may_write(&f) && f.count == 0);
    BlNotice dl = notice("dl-3", BlNotice_Progress, "Downloading");
    snprintf(dl.detail, sizeof(dl.detail), "Game \"X\", 2 of 3");
    snprintf(dl.route, sizeof(dl.route), "downloads/queue");
    dl.progress = 40;
    CHECK(bl_notices_put(&f, &dl));
    BlNotice err = notice("dl-2", BlNotice_Error, "Download failed");
    err.expires = 1792003600;
    CHECK(bl_notices_put(&f, &err));
    static char out[8192];
    size_t n = bl_notices_write(&f, out, sizeof(out));
    CHECK(n > 0 && contains(out, "\"schema\":1") && contains(out, "\"module\":\"playshelf\""));
    CHECK(contains(out, "\"level\":\"progress\"") && contains(out, "\"progress\":40") && contains(out, "Game \\\"X\\\""));
    CHECK(!contains(out + (strstr(out, "dl-2") - out), "\"progress\""));   // none for the error
    CHECK(bl_notices_write(&f, out, 64) == 0);   // too small
    n = bl_notices_write(&f, out, sizeof(out));

    bl_notices_init(&g, "", "");
    CHECK(bl_notices_parse(&g, out, n, toks, BL_NOTICES_TOKENS));
    CHECK(!strcmp(g.module, "playshelf") && !strcmp(g.nro, "switch/PlayShelf/PlayShelf.nro") && g.count == 2);
    const BlNotice *r = bl_notices_find(&g, "dl-3");
    CHECK(r && r->level == BlNotice_Progress && r->progress == 40 && !strcmp(r->detail, "Game \"X\", 2 of 3"));
    CHECK(r && !strcmp(r->route, "downloads/queue") && r->updated == 1792000000 && !r->expires);
    r = bl_notices_find(&g, "dl-2");
    CHECK(r && r->level == BlNotice_Error && r->progress == -1 && r->expires == 1792003600);

    // Replaced by id, removed, pruned.
    dl.progress = 60;
    CHECK(bl_notices_put(&g, &dl) && g.count == 2 && bl_notices_find(&g, "dl-3")->progress == 60);
    CHECK(bl_notices_prune(&g, 1792003599) == 0 && bl_notices_prune(&g, 1792003600) == 1 && g.count == 1);
    CHECK(bl_notices_remove(&g, "dl-3") && !bl_notices_remove(&g, "dl-3") && g.count == 0);
    for (int i = 0; i < BL_NOTICES_MAX; i++) {
        char id[8];
        snprintf(id, sizeof(id), "n%d", i);
        BlNotice x = notice(id, BlNotice_Info, "x");
        CHECK(bl_notices_put(&g, &x));
    }
    BlNotice more = notice("more", BlNotice_Info, "x");
    CHECK(!bl_notices_put(&g, &more));   // full
}

static void test_unknown(void)
{
    // A newer version's members, of the file and of a notice, written back
    // as read; a notice of an unknown level, without an id or twice the same
    // id, left out; a bad route dropped.
    const char text[] =
        "{\"schema\":1,\"module\":\"playguard\",\"nro\":\"switch/PlayGuard/PlayGuard.nro\",\"future\":{\"x\":[1,2]},"
        "\"notices\":[{\"id\":\"pin\",\"level\":\"todo\",\"title\":\"Set a PIN\",\"lang\":\"fr\",\"updated\":5,"
        "\"route\":\"security/pin\",\"icon\":\"lock\"},"
        "{\"id\":\"new\",\"level\":\"celebrate\",\"title\":\"?\"},{\"level\":\"info\",\"title\":\"no id\"},"
        "{\"id\":\"pin\",\"level\":\"info\",\"title\":\"twice\"},"
        "{\"id\":\"bad\",\"level\":\"info\",\"route\":\"../x\"},7]}";
    static BlNotices f;
    bl_notices_init(&f, NULL, NULL);
    CHECK(bl_notices_parse(&f, text, strlen(text), toks, BL_NOTICES_TOKENS));
    CHECK(f.count == 2 && !strcmp(f.items[0].id, "pin") && !strcmp(f.items[1].id, "bad") && !f.items[1].route[0]);
    CHECK(!strcmp(f.items[0].kept, "\"icon\":\"lock\"") && !strcmp(f.kept, "\"future\":{\"x\":[1,2]}"));
    static char out[4096];
    const size_t n = bl_notices_write(&f, out, sizeof(out));
    CHECK(n && contains(out, "\"icon\":\"lock\"}") && contains(out, ",\"future\":{\"x\":[1,2]}}"));
    // Replacing the notice keeps what it did not know.
    BlNotice pin = notice("pin", BlNotice_Todo, "Set a PIN now");
    CHECK(bl_notices_put(&f, &pin) && !strcmp(f.items[0].kept, "\"icon\":\"lock\""));
    // What it writes reads again.
    static BlNotices g;
    bl_notices_init(&g, NULL, NULL);
    CHECK(bl_notices_parse(&g, out, n, toks, BL_NOTICES_TOKENS) && g.count == 2 && !strcmp(g.kept, f.kept));
}

static void test_schema(void)
{
    static BlNotices f;
    // Newer: no notices, never written over.
    const char newer[] = "{\"schema\":2,\"notices\":[{\"id\":\"a\",\"level\":\"todo\",\"title\":\"x\"}]}";
    bl_notices_init(&f, "playshelf", "switch/PlayShelf/PlayShelf.nro");
    CHECK(bl_notices_parse(&f, newer, strlen(newer), toks, BL_NOTICES_TOKENS));
    CHECK(f.newer && f.schema == 2 && f.count == 0 && !bl_notices_may_write(&f));
    const char odd[] = "{\"schema\":\"two\",\"notices\":[]}";
    CHECK(bl_notices_parse(&f, odd, strlen(odd), toks, BL_NOTICES_TOKENS) && f.newer);
    // No schema: 1.
    const char old[] = "{\"notices\":[{\"id\":\"a\",\"level\":\"todo\",\"title\":\"x\"}]}";
    CHECK(bl_notices_parse(&f, old, strlen(old), toks, BL_NOTICES_TOKENS) && !f.newer && f.schema == 1 && f.count == 1);
    // Not a file: empty, false.
    CHECK(!bl_notices_parse(&f, "[]", 2, toks, BL_NOTICES_TOKENS) && f.count == 0);
    CHECK(!bl_notices_parse(&f, "{\"notices\":", 11, toks, BL_NOTICES_TOKENS) && f.count == 0);
    CHECK(!bl_notices_parse(&f, "", 0, toks, BL_NOTICES_TOKENS));
}

static void test_shown(void)
{
    static BlNotices f;
    bl_notices_init(&f, "playshelf", "switch/PlayShelf/PlayShelf.nro");
    BlNotice a = notice("a", BlNotice_Todo, "x"), b = notice("b", BlNotice_Error, "x"), c = notice("c", BlNotice_Info, "x");
    BlNotice d = notice("d", BlNotice_Todo, "x"), e = notice("e", BlNotice_Todo, "x");
    d.expires = 100;
    snprintf(e.lang, sizeof(e.lang), "fr");
    bl_notices_put(&f, &a), bl_notices_put(&f, &b), bl_notices_put(&f, &c), bl_notices_put(&f, &d), bl_notices_put(&f, &e);
    CHECK(bl_notices_attention(&f, 200, "en") == 2);   // a and b: c is information, d expired, e in French
    CHECK(bl_notices_attention(&f, 50, "en") == 3);
    CHECK(bl_notices_attention(&f, 200, "fr") == 1);   // e alone
    e.lang[0] = '\0';
    CHECK(bl_notice_shown(&e, 200, "ja"));

    BlNotice w = notice("p", BlNotice_Progress, "x");
    w.progress = 40;
    w.updated = 1000;
    CHECK(!bl_notice_progress_due(&w, 42, 1010) && bl_notice_progress_due(&w, 45, 1010));
    CHECK(bl_notice_progress_due(&w, 41, 1030) && bl_notice_progress_due(&w, 100, 1001));
    CHECK(bl_notice_progress_due(NULL, 1, 0));
    BlNoticeLevel level;
    CHECK(bl_notice_level_parse("error", &level) && level == BlNotice_Error && !bl_notice_level_parse("x", &level));
    CHECK(!strcmp(bl_notice_level_name(BlNotice_Todo), "todo") && !strcmp(bl_notice_level_name((BlNoticeLevel)9), "info"));
}

int main(void)
{
    test_reader();
    test_strings();
    test_names();
    test_round_trip();
    test_unknown();
    test_schema();
    test_shown();
    return CHECK_DONE("notices: all tests passed");
}
