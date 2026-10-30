// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bl_kv.h"

// names.txt past this is started again with the icons (each game's name
// once: some 300 KB for every game of BL_CACHE_ICONS_MAX_FILES).
#define NAMES_MAX_BYTES (512u << 10)
// A path under the root.
#define PATH_MAX_ (BL_CACHE_ROOT_MAX + 64)
// A line of names.txt: "<16 hex>=<name>\n".
#define LINE_MAX_ (16 + 1 + BL_CACHE_NAME_MAX + 2)

static bool under(const char *root, const char *rel, char *out, size_t cap)
{
    const size_t n = strlen(root);
    const int w = snprintf(out, cap, "%s%s%s", root, n && root[n - 1] == '/' ? "" : "/", rel);
    return w > 0 && (size_t)w < cap;
}

// mkdir -p of `path`, from its byte `from` on (the root's own folders are
// not made: "sdmc:" is not one).
static bool make_dirs(char *path, size_t from)
{
    for (char *p = path + from;; p++) {
        if (*p != '/' && *p != '\0') continue;
        const char c = *p;
        *p = '\0';
        const bool ok = mkdir(path, 0777) == 0 || errno == EEXIST;
        *p = c;
        if (!ok) return false;
        if (c == '\0') return true;
    }
}

// A new file for writing, readable by all but writable by its owner only
// (fopen "wb" would ask for 0666).
static FILE *create_file(const char *path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
}

// `tmp` made `path`: the console's rename does not replace a file.
static bool replace_with(const char *tmp, const char *path)
{
    remove(path);
    if (rename(tmp, path) == 0) return true;
    remove(tmp);
    return false;
}

static bool write_file(const char *path, const char *tmp, const void *data, size_t size)
{
    FILE *f = create_file(tmp);
    if (!f) return false;
    const bool written = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0 || !written) {
        remove(tmp);
        return false;
    }
    return replace_with(tmp, path);
}

static bool looks_like_jpeg(const unsigned char *p, size_t size)
{
    return size >= 4 && size <= BL_CACHE_ICON_MAX && p[0] == 0xFF && p[1] == 0xD8;
}

// "0100000000010000" + `ext`: the ID in `*id`.
static bool icon_file(const char *name, const char *ext, uint64_t *id)
{
    if (strlen(name) != 16 + strlen(ext) || strcmp(name + 16, ext) != 0) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const char ch = name[i];
        const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    if (id) *id = v;
    return true;
}

// Counts the icons (and removes the files cut by a crash), or removes them
// all: how many it removed.
static uint32_t scan_once(BlCache *c, const char *dir, bool remove_all)
{
    c->bytes = 0;
    c->files = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    char path[PATH_MAX_ + 32];
    uint32_t removed = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const bool icon = icon_file(e->d_name, ".jpg", NULL);
        if (!icon && !icon_file(e->d_name, ".tmp", NULL)) continue;
        const size_t dl = strlen(dir), nl = strlen(e->d_name);   // nl: 20, from icon_file()
        if (dl + 1 + nl >= sizeof(path)) continue;
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1, e->d_name, nl + 1);
        struct stat st;
        if (remove_all || !icon) {
            if (remove(path) == 0) removed++;
        } else if (stat(path, &st) == 0) {
            c->bytes += (uint64_t)st.st_size;
            c->files++;
        }
    }
    closedir(d);
    return removed;
}

static void scan(BlCache *c, const char *dir, bool remove_all)
{
    // Again while it deletes: a listing may skip entries deleted under it.
    for (int pass = 0; pass < 4 && scan_once(c, dir, remove_all) > 0; pass++) {}
}

typedef struct {
    BlKvFile *file;
    uint32_t  lang;
    bool      has_lang;
} ConfRead;

static void on_conf(void *ctx, const char *key, const char *value)
{
    ConfRead *r = (ConfRead *)ctx;
    if (!strcmp(key, "lang")) r->has_lang = bl_parse_uint(value, 1, 255, &r->lang);
    else bl_kv_keep(r->file, key, value);
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

bool bl_cache_open(BlCache *c, const char *root, uint8_t lang)
{
    memset(c, 0, sizeof(*c));
    char dir[PATH_MAX_], conf[PATH_MAX_], names[PATH_MAX_];
    if (!root || !root[0] || strlen(root) >= sizeof(c->root) || !lang) return false;
    strcpy(c->root, root);
    c->lang = lang;
    if (!under(root, BL_CACHE_ICONS_DIR, dir, sizeof(dir)) || !under(root, BL_CACHE_FILE, conf, sizeof(conf)) ||
        !under(root, BL_CACHE_NAMES_FILE, names, sizeof(names)))
        return false;

    // cache.conf: its schema and its language.
    char text[1024];
    size_t len = 0;
    bool had = false;
    FILE *f = fopen(conf, "rb");
    if (f) {
        len = fread(text, 1, sizeof(text) - 1, f);
        had = feof(f) != 0;
        if (!had) len = 0;   // too long to be one: written again
        fclose(f);
    }
    text[len] = '\0';
    BlKvFile file;
    bl_kv_file_init(&file, c->kept, sizeof(c->kept));
    ConfRead read = { &file, 0, false };
    bl_kv_read(&file, text, len, on_conf, &read);
    if (had && !bl_kv_may_rewrite(&file, BL_CACHE_SCHEMA)) {
        if (file.schema == 0 || file.schema > BL_CACHE_SCHEMA) return false;   // a newer version's: left alone
        c->kept[0] = '\0';                                                    // a line lost: written anew
    }

    if (!make_dirs(dir, strlen(root))) return false;
    const bool same = had && read.has_lang && read.lang == lang;
    if (same) scan(c, dir, false);
    // Another language, or full: start again with the games read now.
    const long names_size = file_size(names);
    if (!same || c->bytes >= BL_CACHE_ICONS_MAX_BYTES || c->files >= BL_CACHE_ICONS_MAX_FILES ||
        names_size > (long)NAMES_MAX_BYTES) {
        scan(c, dir, true);
        c->bytes = 0;
        c->files = 0;
        remove(names);
    }
    if (!same) {
        char out[1024 + BL_CACHE_KEPT_MAX];
        int n = snprintf(out, sizeof(out), "%s=%u\nlang=%u\n", BL_KV_SCHEMA_KEY, BL_CACHE_SCHEMA, (unsigned)lang);
        size_t total = n > 0 ? bl_kv_write_kept(&file, out, sizeof(out), (size_t)n) : 0;
        char tmp[PATH_MAX_ + 4];
        if (!total || snprintf(tmp, sizeof(tmp), "%s.tmp", conf) >= (int)sizeof(tmp) ||
            !write_file(conf, tmp, out, total))
            return false;
    }
    c->ready = true;
    return true;
}

bool bl_cache_icon_path(const char *root, uint64_t id, char *out, size_t cap)
{
    char rel[sizeof(BL_CACHE_ICONS_DIR) + 24];
    snprintf(rel, sizeof(rel), "%s/%016llX.jpg", BL_CACHE_ICONS_DIR, (unsigned long long)id);
    return root && under(root, rel, out, cap);
}

size_t bl_cache_icon_get(const BlCache *c, uint64_t id, void *buf, size_t cap)
{
    char path[PATH_MAX_ + 32];
    if (!c->ready || !buf || !bl_cache_icon_path(c->root, id, path, sizeof(path))) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    const bool whole = feof(f) || (n == cap && fgetc(f) == EOF);
    fclose(f);
    return whole && looks_like_jpeg((const unsigned char *)buf, n) ? n : 0;
}

bool bl_cache_icon_put(BlCache *c, uint64_t id, const void *jpeg, size_t size)
{
    char path[PATH_MAX_ + 32], tmp[PATH_MAX_ + 32];
    if (!c->ready || !jpeg || !looks_like_jpeg((const unsigned char *)jpeg, size) ||
        c->files >= BL_CACHE_ICONS_MAX_FILES || c->bytes + size > BL_CACHE_ICONS_MAX_BYTES ||
        !bl_cache_icon_path(c->root, id, path, sizeof(path)))
        return false;
    memcpy(tmp, path, strlen(path) - 4);
    strcpy(tmp + strlen(path) - 4, ".tmp");
    const long before = file_size(path);
    if (!write_file(path, tmp, jpeg, size)) return false;
    if (before >= 0) {
        c->bytes -= (uint64_t)before <= c->bytes ? (uint64_t)before : c->bytes;
    } else {
        c->files++;
    }
    c->bytes += size;
    return true;
}

// The ID of a names.txt line ("<16 hex>=…"), and where its name starts.
static bool name_line(const char *line, uint64_t *id, const char **name)
{
    char key[17];
    if (strlen(line) < 17 || line[16] != '=') return false;
    memcpy(key, line, 16);
    key[16] = '\0';
    char probe[21];
    snprintf(probe, sizeof(probe), "%s.jpg", key);
    if (!icon_file(probe, ".jpg", id)) return false;
    *name = line + 17;
    return true;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

// names.txt opened for reading, when it is of this schema (its first line).
static FILE *open_names(const BlCache *c, char *line, size_t cap)
{
    char path[PATH_MAX_];
    if (!under(c->root, BL_CACHE_NAMES_FILE, path, sizeof(path))) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t schema = 0;
    if (!fgets(line, (int)cap, f) || (chomp(line), strncmp(line, "schema=", 7) != 0) ||
        !bl_parse_uint(line + 7, 1, 0xFFFFFFFFu, &schema) || schema != BL_CACHE_SCHEMA) {
        fclose(f);
        return NULL;
    }
    return f;
}

bool bl_cache_name_get(const BlCache *c, uint64_t id, char *out, size_t cap)
{
    char line[LINE_MAX_];
    if (!c->ready || !out || !cap) return false;
    FILE *f = open_names(c, line, sizeof(line));
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        chomp(line);
        uint64_t got;
        const char *name;
        if (!name_line(line, &got, &name) || got != id || !name[0]) continue;
        const size_t n = strlen(name);
        if (n < cap) {
            memcpy(out, name, n + 1);
            found = true;
        }
    }
    fclose(f);
    return found;
}

static bool name_ok(const char *name)
{
    return name && name[0] && strlen(name) < BL_CACHE_NAME_MAX && !strpbrk(name, "\r\n");
}

bool bl_cache_names_put(BlCache *c, const uint64_t *ids, const char *const *names, size_t count)
{
    char path[PATH_MAX_], tmp[PATH_MAX_ + 4], line[LINE_MAX_];
    if (!c->ready || (count && (!ids || !names)) || !under(c->root, BL_CACHE_NAMES_FILE, path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return false;
    // A names.txt of another schema: a newer version's, left alone.
    FILE *in = fopen(path, "rb");
    if (in) {
        fclose(in);
        in = open_names(c, line, sizeof(line));
        if (!in) return false;
    }
    FILE *out = create_file(tmp);
    if (!out) {
        if (in) fclose(in);
        return false;
    }
    bool ok = fprintf(out, "%s=%u\n", BL_KV_SCHEMA_KEY, BL_CACHE_SCHEMA) > 0;
    // The lines kept, minus the games given; the lines it does not know as read.
    while (ok && in && fgets(line, sizeof(line), in)) {
        const size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(in)) {
            // Longer than a line can be: not one of ours, dropped with its rest.
            for (int ch; (ch = fgetc(in)) != EOF && ch != '\n';) {}
            continue;
        }
        chomp(line);
        uint64_t id;
        const char *name;
        bool given = false;
        if (name_line(line, &id, &name))
            for (size_t i = 0; i < count && !given; i++) given = ids[i] == id && name_ok(names[i]);
        if (!given && line[0]) ok = fprintf(out, "%s\n", line) > 0;
    }
    for (size_t i = 0; ok && i < count; i++) {
        if (!name_ok(names[i])) continue;
        bool later = false;   // the last one given for an ID wins
        for (size_t j = i + 1; j < count && !later; j++) later = ids[j] == ids[i] && name_ok(names[j]);
        if (!later) ok = fprintf(out, "%016llX=%s\n", (unsigned long long)ids[i], names[i]) > 0;
    }
    if (in) fclose(in);
    if (fclose(out) != 0 || !ok) {
        remove(tmp);
        return false;
    }
    return replace_with(tmp, path);
}
