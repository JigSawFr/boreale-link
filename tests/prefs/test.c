// bl_prefs: the look the homebrews share, read and written back with the
// keys a version does not know, a newer schema never written over.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <string.h>

#include "bl_prefs.h"
#include "check.h"

static void test_round_trip(void)
{
    BlPrefs p;
    bl_prefs_defaults(&p);
    for (int i = 0; i < BlPref_Count; i++) CHECK(bl_prefs_get(&p, (BlPref)i) == NULL);
    char out[1024];
    size_t n = bl_prefs_write(&p, out, sizeof(out));
    CHECK(n && strstr(out, "schema=1\n") && !strstr(out, "language="));   // nothing set: only the schema

    CHECK(bl_prefs_set(&p, BlPref_Language, "fr-CA") && bl_prefs_set(&p, BlPref_Theme, "dark"));
    CHECK(bl_prefs_set(&p, BlPref_TextSize, "large"));
    CHECK(!bl_prefs_set(&p, BlPref_Effects, "not ok") && bl_prefs_get(&p, BlPref_Effects) == NULL);   // a space
    CHECK(!bl_prefs_set(&p, BlPref_Count, "x"));
    n = bl_prefs_write(&p, out, sizeof(out));
    CHECK(n && strstr(out, "language=fr-CA\ntheme=dark\ntext_size=large\n") && !strstr(out, "effects="));

    BlPrefs q;
    bl_prefs_defaults(&q);
    bl_prefs_parse(&q, out, n);
    CHECK(!strcmp(bl_prefs_get(&q, BlPref_Language), "fr-CA") && !strcmp(bl_prefs_get(&q, BlPref_Theme), "dark"));
    CHECK(!strcmp(bl_prefs_get(&q, BlPref_TextSize), "large") && bl_prefs_get(&q, BlPref_Density) == NULL);
    CHECK(bl_prefs_may_write(&q));

    // Unset again.
    CHECK(bl_prefs_set(&q, BlPref_Theme, "") && bl_prefs_get(&q, BlPref_Theme) == NULL);
    CHECK(bl_prefs_set(&q, BlPref_TextSize, NULL) && bl_prefs_get(&q, BlPref_TextSize) == NULL);
    CHECK(!strcmp(bl_prefs_key(BlPref_TextSize), "text_size") && !strcmp(bl_prefs_key(BlPref_Count), ""));

    // Too small: nothing.
    CHECK(bl_prefs_write(&p, out, 20) == 0);
}

static void test_kept_and_bad(void)
{
    const char *text = "schema=1\nlanguage=ja\ntheme=bad value\nfuture=1\ndensity=" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                       "effects=reduced\n";
    BlPrefs p;
    bl_prefs_defaults(&p);
    bl_prefs_parse(&p, text, strlen(text));
    CHECK(!strcmp(bl_prefs_get(&p, BlPref_Language), "ja") && !strcmp(bl_prefs_get(&p, BlPref_Effects), "reduced"));
    CHECK(bl_prefs_get(&p, BlPref_Theme) == NULL && bl_prefs_get(&p, BlPref_Density) == NULL);   // left unset
    char out[1024];
    const size_t n = bl_prefs_write(&p, out, sizeof(out));
    CHECK(n && strstr(out, "language=ja\neffects=reduced\nfuture=1\n"));   // the unknown key kept, after
    // A copy keeps its own lines.
    BlPrefs copy = p;
    char out2[1024];
    CHECK(bl_prefs_write(&copy, out2, sizeof(out2)) == n && !strcmp(out, out2));
}

static void test_schema(void)
{
    BlPrefs p;
    bl_prefs_defaults(&p);
    const char *newer = "schema=2\nlanguage=de\n";
    bl_prefs_parse(&p, newer, strlen(newer));
    CHECK(!bl_prefs_may_write(&p) && !strcmp(bl_prefs_get(&p, BlPref_Language), "de"));   // read, not written over
    bl_prefs_defaults(&p);
    const char *none = "theme=light\n";   // no schema line: the first one
    bl_prefs_parse(&p, none, strlen(none));
    CHECK(bl_prefs_may_write(&p) && !strcmp(bl_prefs_get(&p, BlPref_Theme), "light"));
}

int main(void)
{
    test_round_trip();
    test_kept_and_bad();
    test_schema();
    return CHECK_DONE("bl_prefs: round trip, kept keys, bad values and schema assertions passed");
}
