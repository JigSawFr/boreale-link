// boreale-link — keeps the console's remote link (MQTT, Home Assistant) up
// while PlayGuard is closed: the same session engine as the app
// (source/sync/sync_engine.c), the console read and changed through the same
// service layer (source/core/), orders carried out through the same gate,
// unlock and relock (source/sync/sync_exec.c), and only under the "auto"
// policy; with "ask" they wait for PlayGuard.
//
// While PlayGuard runs it holds a pg:agent session (agent_ipc.c): the agent
// stays the only MQTT client, publishes what PlayGuard pushes, and hands it
// the orders; in the foreground PlayGuard is the only one to read pctl.
//
// Files (sd:/config/playguard/, as PlayGuard's): sync.conf (the settings,
// PlayGuard's Remote access screen) and sync/profiles.txt, shared by every
// system on the SD card; in this system's own folder, nand/<system>/sync/
// (core/nand_key.h): nro_state.txt (what PlayGuard last saved: records,
// read-only) and names.txt, and, written here, agent_state.txt (the records of
// what the agent changed) and agent_events.log (its orders, imported into
// PlayGuard's history); logs/reports/report_*.txt (reports asked for, named
// and pruned as PlayGuard's).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "agent_core.h"
#include "agent_relay.h"
#include "bl_app.h"
#include "bl_cache.h"
#include "bl_relay.h"
#include "bl_net.h"
#include "bl_tls.h"
#include "core/calendar.h"
#include "console_info.h"
#include "nand_key.h"
#include "pctl_ops.h"
#include "playstats.h"
#include "sync_engine.h"
#include "sync_icons.h"
#include "sync_state.h"
#include "sync_system.h"
#include "sync_notices.h"
#include "sync_titles.h"
#include "sync_watch.h"
#include "core/sysinfo.h"
#include "time_ops.h"
#include "core/write_guard.h"

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0"
#endif

#define DATA_DIR     "sdmc:/config/playguard"
#define CONF_PATH    DATA_DIR "/sync.conf"
#define LINK_PATH    "sdmc:/" BL_LINK_CONF_FILE   // the broker's settings, every Boréale homebrew's
#define SYNC_DIR     DATA_DIR "/sync"
#define PROFILES     SYNC_DIR "/profiles.txt"
#define REPORTS_DIR  DATA_DIR "/logs/reports"
#define ICON_CACHE   "sdmc:/" BL_CACHE_ICONS_DIR   // the homebrews' shared icons (bl_cache), read only

// This system's (nand/<key>/sync/), once the system is known (system_paths()).
static char g_system_dir[64];
static char NRO_STATE[96], AGENT_STATE[96], AGENT_EVENTS[96], NAMES[96], INTEGRATION[96], AGENT_WATCHDOG[96];
static char ICONS[96], AGENT_ICONS[96], TITLES[96];
static bool g_emummc;

#define ACTIVITY_EVERY_MS (5 * 60 * 1000ULL)
#define CONF_CHECK_MS     (30 * 1000ULL)

// Where the loop is, and when it last turned (the system tick): what the
// watchdog says when a call never returns (seen on a console, the link then
// showed its last status for hours). Each part that calls a service names
// itself, and puts back the part it was called from.
static const char PHASE_LINK[] = "the link (network)";
static const char PHASE_HANDOVER[] = "the hand-over (network)";
static const char PHASE_ICON[] = "the game's icon (SD card, network)";
static const char *volatile g_phase = "start-up";
static volatile uint64_t g_turn_ms;

static const char *phase(const char *p)
{
    const char *was = g_phase;
    g_phase = p;
    return was;
}

// The wall clock read once a turn: a line logged, the watchdog's too, starts
// with its local time without calling the time service (it may be the call
// that hangs).
static volatile u64 g_wall_posix, g_wall_ms;
static volatile s32 g_wall_offset;   // local time minus UTC, in seconds

static void wall_read(uint64_t now_ms)
{
    u64 posix = 0;
    LocalTime t;
    if (!time_local_now(&posix, &t)) return;
    g_wall_offset = (s32)((s64)t.hour * 3600 + t.minute * 60 + t.second - (s64)(posix % 86400));
    g_wall_ms = now_ms;
    g_wall_posix = posix;
}

// The local date and time from it, "YYYY-MM-DD HH:MM:SS"; false while unknown.
static bool wall_stamp(char out[32])
{
    const u64 wall = g_wall_posix;
    if (!wall) return false;
    const uint64_t now = bl_now_ms(), read = g_wall_ms;
    const s64 local = (s64)(wall + (now > read ? (now - read) / 1000 : 0)) + g_wall_offset;
    const s64 days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    const unsigned s = (unsigned)(local - days * 86400);
    int y = 0;
    unsigned m = 0, d = 0;
    calendar_civil_from_days(days, &y, &m, &d);
    snprintf(out, 32, "%04d-%02u-%02u %02u:%02u:%02u", y, m, d, s / 3600, s / 60 % 60, s % 60);
    return true;
}

// A sysmodule: no applet, one fs session, a heap of its own.
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x180000
static char g_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = g_heap;
    fake_heap_end   = g_heap + INNER_HEAP_SIZE;
}

static bool s_sd_mounted;

void __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    // The firmware version (the service layer gates commands on it), and
    // Atmosphère's mark (bit 31), as hbloader gives an application.
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            u32 v = MAKEHOSVERSION(fw.major, fw.minor, fw.micro);
            if (R_SUCCEEDED(splInitialize())) {
                u64 ams = 0;
                if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &ams))) v |= BIT(31);
                splExit();
            }
            hosversionSet(v);
        }
        setsysExit();
    }
    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    s_sd_mounted = R_SUCCEEDED(fsdevMountSdmc());
    timeInitialize();
}

void __appExit(void)
{
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---- shared with the IPC thread (agent_ipc.c) ----

AgentShared g_shared;
AgentRelay  g_relay;     // the bl:link sessions, under g_lock as well
Mutex       g_lock;
void        agent_ipc_start(void);

static void slog(const char *fmt, const char *a, const char *b)
{
    char line[AGENT_LOG_LINE], stamp[32];
    int at = 0;
    if (wall_stamp(stamp)) at = snprintf(line, sizeof(line), "%s ", stamp + 11);   // the time of day
    snprintf(line + at, sizeof(line) - (size_t)at, fmt, a ? a : "", b ? b : "");
    mutexLock(&g_lock);
    agent_log(&g_shared, line);
    mutexUnlock(&g_lock);
}

// ---- files ----

static size_t read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return n;
}

// A file opened for writing, created owner-writable only (fopen would ask
// for 0666; the SD card's FAT keeps no modes, but nothing should ask for it).
static FILE *open_write(const char *path, bool append)
{
    const int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, append ? "ab" : "wb");
    if (!f) close(fd);
    return f;
}

static bool write_file(const char *path, const char *text, size_t len)
{
    // As PlayGuard's paths::atomic_write (sync.conf is written by both):
    // "<path>.part", renamed "<path>.tmp" once whole, so a ".tmp" is always
    // a complete file and a write cut short leaves only a ".part".
    char tmp[160], part[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(part, sizeof(part), "%s.part", path);
    struct stat st;
    // A write stopped between removing the file and renaming its ".tmp":
    // that ".tmp" is the only good copy, put in place first.
    if (stat(path, &st) != 0 && stat(tmp, &st) == 0) rename(tmp, path);
    FILE *f = open_write(part, false);
    if (!f) return false;
    // On the card before the old file goes: a power loss cannot leave an
    // empty one.
    const bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0 || !ok) {
        remove(part);
        return false;
    }
    // An older ".tmp" next to the file goes (FAT: no rename over a file);
    // without the file it is kept, and this write fails rather than lose it.
    if (stat(path, &st) == 0) remove(tmp);
    if (rename(part, tmp) != 0) {
        remove(part);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

static uint64_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_mtime : 0;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// The SD card at boot may come a moment after the agent: retried for about
// 10 s (as the recovery sysmodule does), then again by the main loop.
static bool wait_for_sd(int tries)
{
    for (int i = 0; i < tries && !s_sd_mounted; i++) {
        svcSleepThread(250000000ULL);
        s_sd_mounted = R_SUCCEEDED(fsdevMountSdmc());
    }
    return s_sd_mounted;
}

// The folders, each level in turn (mkdir makes one).
static void make_dirs(void)
{
    mkdir("sdmc:/config", 0777);
    mkdir(DATA_DIR, 0777);
    mkdir(SYNC_DIR, 0777);
    mkdir(DATA_DIR "/nand", 0777);
    mkdir(g_system_dir, 0777);
    char sync_dir[80];
    snprintf(sync_dir, sizeof(sync_dir), "%s/sync", g_system_dir);
    mkdir(sync_dir, 0777);
}

// This system's folder, named as PlayGuard names it (util/nand.hpp).
static void system_paths(void)
{
    static SysInfo si;
    sysinfo_get(&si);
    const SysStorage storage = sysinfo_storage(&si);
    g_emummc = storage == SysStorage_EmuMMC;
    snprintf(g_system_dir, sizeof(g_system_dir), DATA_DIR "/nand/%s",
             nand_key_name(storage != SysStorage_Unknown, g_emummc));
    snprintf(NRO_STATE, sizeof(NRO_STATE), "%s/sync/nro_state.txt", g_system_dir);
    snprintf(AGENT_STATE, sizeof(AGENT_STATE), "%s/sync/agent_state.txt", g_system_dir);
    snprintf(AGENT_EVENTS, sizeof(AGENT_EVENTS), "%s/sync/agent_events.log", g_system_dir);
    snprintf(NAMES, sizeof(NAMES), "%s/sync/names.txt", g_system_dir);
    snprintf(INTEGRATION, sizeof(INTEGRATION), "%s/sync/integration.json", g_system_dir);
    snprintf(AGENT_WATCHDOG, sizeof(AGENT_WATCHDOG), "%s/sync/agent_watchdog.log", g_system_dir);
    snprintf(ICONS, sizeof(ICONS), "%s/sync/icons.txt", g_system_dir);
    snprintf(AGENT_ICONS, sizeof(AGENT_ICONS), "%s/sync/agent_icons.txt", g_system_dir);
    snprintf(TITLES, sizeof(TITLES), "%s/sync/titles.txt", g_system_dir);
}

// ---- the agent's state ----

static SyncConf      g_conf;
static bool          g_conf_ok;
static uint64_t      g_conf_mtime, g_link_mtime, g_nro_mtime, g_conf_checked;
static BlLinkConf    g_link;
static AgentNroState g_nro;
static SyncRecords   g_records;
static char          g_firmware[16];
static char          g_today[11];
static SyncEngine    g_engine;
static bool          g_engine_on;
static BlTcp         g_tcp;
static BlTls         g_tls;
static bool          g_sockets;
static bool          g_stopped;   // asked to leave (bl:link's Shutdown): offline until Resume or the process ends

static char     g_text[0x4000];          // files read
static char     g_doc[AGENT_DOC_LARGE];  // documents taken from the shared state
static char     g_activity[AGENT_DOC_STATE];
static size_t   g_activity_len;
static uint64_t g_activity_at;
static uint32_t g_activity_s;            // today's total, for the state
static bool     g_activity_ok;
static u64      g_playing, g_playing_since;
static u64      g_waiting;               // none in front: the one suspended in the background
static bool     g_front_ok;              // the last read of what is in front worked
static PlayLogTotal g_totals[PLAYSTATS_MAX];
static size_t   g_n_totals;
static PlayLogAccountDays g_accounts[PLAYSTATS_MAX_ACCOUNTS];
static size_t   g_n_accounts;            // SIZE_MAX: not known
static u8       g_day_wday[7];

static void load_conf(void)
{
    sync_conf_defaults(&g_conf);
    const size_t n = read_file(CONF_PATH, g_text, sizeof(g_text));
    g_conf_ok = n > 0 && sync_conf_parse(&g_conf, g_text, n);
    // The broker's settings are link.conf's alone.
    const size_t l = read_file(LINK_PATH, g_text, sizeof(g_text));
    bl_link_conf_defaults(&g_link);
    if (l > 0) {
        bl_link_conf_parse(&g_link, g_text, l);
        sync_conf_from_link(&g_conf, &g_link);
        g_conf_ok = true;
    }
    g_conf.emummc = g_emummc;   // this system's device (sync_conf_device_id)
    g_conf_mtime = mtime_of(CONF_PATH);
    g_link_mtime = mtime_of(LINK_PATH);
}

// PlayGuard's module: carried only while its registry file
// (config/boreale/apps/playguard.conf, written by PlayGuard) unlocks it and
// its .nro is still on the SD card. Without it the agent publishes the
// console's own documents and relays, reads no parental controls, takes no
// order of PlayGuard's and does not announce it.
static bool g_module_on;

static bool module_read(void)
{
    static BlApp app;
    char rel[BL_APP_PATH_MAX], path[BL_APP_PATH_MAX + 8];
    if (!bl_app_path("playguard", rel, sizeof(rel))) return false;
    snprintf(path, sizeof(path), "sdmc:/%s", rel);
    const size_t n = read_file(path, g_text, sizeof(g_text));
    bl_app_defaults(&app);
    if (n) bl_app_parse(&app, g_text, n);
    if (!bl_app_unlocks(&app)) return false;
    snprintf(path, sizeof(path), "sdmc:/%s", app.nro);
    return exists(path);
}

// Off only after a few reads in a row: a card that did not answer once must
// not clear the module's documents from the broker.
#define MODULE_OFF_READS 3

static void module_refresh(void)
{
    static unsigned off_reads;
    const bool on = module_read();
    off_reads = on ? 0 : off_reads + 1;
    if (on == g_module_on || (!on && off_reads < MODULE_OFF_READS)) return;
    g_module_on = on;
    if (g_engine_on) sync_engine_set_module(&g_engine, on);
    slog(on ? "PlayGuard's module is on%s%s" : "PlayGuard's module is off%s%s", NULL, NULL);
}

static void save_records(void)
{
    char out[512];
    const size_t n = sync_records_write(&g_records, out, sizeof(out));
    if (n) write_file(AGENT_STATE, out, n);
    mutexLock(&g_lock);
    g_shared.records.valid = 1;
    g_shared.records.records = g_records;
    mutexUnlock(&g_lock);
}

// PlayGuard's records when it saved after the agent last did, else the agent's.
static void load_records(void)
{
    const size_t n = read_file(NRO_STATE, g_text, sizeof(g_text));
    agent_nro_parse(&g_nro, n ? g_text : NULL, n);
    g_nro_mtime = mtime_of(NRO_STATE);
    sync_records_clear(&g_records);
    if (g_nro.known && g_nro_mtime >= mtime_of(AGENT_STATE)) {
        g_records = g_nro.records;
    } else {
        const size_t m = read_file(AGENT_STATE, g_text, sizeof(g_text));
        if (m) sync_records_parse(&g_records, g_text, m);
    }
    mutexLock(&g_lock);
    g_shared.records.valid = 1;
    g_shared.records.records = g_records;
    mutexUnlock(&g_lock);
}

static bool may_write(void)
{
    return agent_may_write(&g_nro, g_firmware);
}

// ---- the console ----

// "YYYY-MM-DD" of a local time (`out` holds 11 bytes).
static void date_of(const LocalTime *t, char out[11])
{
    char text[24];
    snprintf(text, sizeof(text), "%04u-%02u-%02u", (unsigned)t->year % 10000u, (unsigned)t->month % 100u,
             (unsigned)t->day % 100u);
    memcpy(out, text, 10);
    out[10] = '\0';
}

static void today_text(char out[11], int *weekday)
{
    LocalTime t;
    u64 now = 0;
    if (time_local_now(&now, &t)) {
        date_of(&t, out);
        if (weekday) *weekday = t.wday;
    } else {
        out[0] = '\0';
        if (weekday) *weekday = -1;
    }
}

// Each account's time on day k (days back), in `out`: NULL when not known.
static const SyncAccountTime *accounts_of(int k, SyncAccountTime out[PLAYSTATS_MAX_ACCOUNTS], size_t *n)
{
    *n = 0;
    if (g_n_accounts == SIZE_MAX) return NULL;
    for (size_t i = 0; i < g_n_accounts; i++) {
        out[i].uid[0] = g_accounts[i].uid[0];
        out[i].uid[1] = g_accounts[i].uid[1];
        out[i].seconds = g_accounts[i].day_s[k];
    }
    *n = g_n_accounts;
    return out;
}

// What each title is (sync_titles.h): PlayGuard's titles.txt, read with
// each read of the play log; without it, or for a title it does not list
// (one installed since PlayGuard last ran), the title's ID says.
static SyncTitles g_titles;

static void load_titles(void)
{
    const size_t n = read_file(TITLES, g_text, sizeof(g_text));
    sync_titles_parse(&g_titles, g_text, n);
}

static void read_activity(uint64_t now_ms, bool force)
{
    if (!force && g_activity_at && now_ms - g_activity_at < ACTIVITY_EVERY_MS) return;
    g_activity_at = now_ms;
    u64 now = 0;
    Result rc = 0;
    const char *was = phase("the play log (pdm)");
    g_n_totals = playstats_days(g_totals, PLAYSTATS_MAX, &now, g_day_wday, &g_playing, &g_playing_since, &g_waiting,
                                g_accounts, PLAYSTATS_MAX_ACCOUNTS, &g_n_accounts, &rc);
    phase(was);
    g_activity_ok = R_SUCCEEDED(rc);
    phase("titles.txt (SD card)");
    load_titles();
    phase(was);
    g_activity_s = 0;
    for (size_t i = 0; i < g_n_totals; i++) g_activity_s += g_totals[i].today_s;
    if (!g_activity_ok) {
        g_activity_len = 0;
        return;
    }
    // Today's document (k = 0).
    static SyncAppTime apps[PLAYSTATS_MAX];
    size_t n = 0;
    for (size_t i = 0; i < g_n_totals; i++)
        if (g_totals[i].today_s) apps[n++] = (SyncAppTime){ g_totals[i].app_id, g_totals[i].today_s };
    char date[11];
    today_text(date, NULL);
    SyncActivity a;
    memset(&a, 0, sizeof(a));
    a.source = "agent";
    a.ts = now;
    a.local_date = date;
    // Only games by their ID: the homebrews' time as one total.
    a.n_apps = sync_titles_games_only(&g_titles, apps, n, &a.homebrew_s);
    a.apps = apps;
    a.now_playing = g_playing;
    a.now_playing_since = g_playing_since;
    a.now_playing_kind = sync_titles_kind(&g_titles, g_playing);
    static SyncAccountTime accounts[PLAYSTATS_MAX_ACCOUNTS];
    a.accounts = accounts_of(0, accounts, &a.n_accounts);
    g_activity_len = sync_activity_build(&a, g_activity, sizeof(g_activity));
}

// A finished day (k days back) from the last read.
static size_t final_doc(int k, const char *date, char *out, size_t cap)
{
    static SyncAppTime apps[PLAYSTATS_MAX];
    size_t n = 0;
    for (size_t i = 0; i < g_n_totals; i++)
        if (g_totals[i].day_s[k]) apps[n++] = (SyncAppTime){ g_totals[i].app_id, g_totals[i].day_s[k] };
    u64 now = 0;
    time_local_now(&now, NULL);
    SyncActivity a;
    memset(&a, 0, sizeof(a));
    a.source = "agent";
    a.ts = now;
    a.local_date = date;
    a.final = true;
    a.n_apps = sync_titles_games_only(&g_titles, apps, n, &a.homebrew_s);
    a.apps = apps;
    static SyncAccountTime accounts[PLAYSTATS_MAX_ACCOUNTS];
    a.accounts = accounts_of(k, accounts, &a.n_accounts);
    return sync_activity_build(&a, out, cap);
}

// The game's name PlayGuard last wrote (names.txt), "" when unknown.
static void name_of(u64 app_id, char *out, size_t cap)
{
    out[0] = '\0';
    if (!app_id) return;
    char id[17];
    snprintf(id, sizeof(id), "%016llX", (unsigned long long)app_id);
    const size_t n = read_file(NAMES, g_text, sizeof(g_text));
    for (size_t at = 0; at < n;) {
        const char *line = g_text + at;
        const char *end = memchr(line, '\n', n - at);
        const size_t len = end ? (size_t)(end - line) : n - at;
        at += len + 1;
        if (len < 18 || line[16] != '=' || strncmp(line, id, 16)) continue;
        size_t name_len = len - 17;
        if (name_len && line[17 + name_len - 1] == '\r') name_len--;
        if (name_len >= cap) name_len = cap - 1;
        memcpy(out, line + 17, name_len);
        out[name_len] = '\0';
        return;
    }
}

static size_t host_profiles(void *ctx, char (*names)[SYNC_PROFILE_MAX], size_t max);
static bool profile_days(void *ctx, const char *name, uint16_t days[7]);

static size_t own_state(char *out, size_t cap)
{
    static PctlStatus st;
    static PtState pt;
    static PtSample sample;
    static SysInfo si;
    const char *was = phase("the console's state (pctl)");
    pctl_status_fetch(&st);
    pctl_play_timer_query(&pt);
    pctl_play_timer_sample(&sample);
    phase(was);
    sysinfo_get(&si);
    char nickname[0x80];
    was = phase("the console's nickname (set:sys)");
    console_nickname_read(nickname);
    phase(was);
    bool accurate = false;
    const bool accurate_ok = R_SUCCEEDED(time_network_accuracy(&accurate));
    char date[11];
    int weekday = -1;
    today_text(date, &weekday);
    u64 now = 0;
    LocalTime local;
    const bool local_ok = time_local_now(&now, &local);
    const SyncStatus *ls = sync_engine_status(&g_engine);

    SyncSnapshot s;
    memset(&s, 0, sizeof(s));
    s.source = "agent";
    s.ts = now;
    s.local_date = date;
    s.weekday = weekday;
    s.local_min_ok = local_ok;
    s.local_min = local_ok ? local.hour * 60 + local.minute : 0;
    s.clock_accurate_ok = accurate_ok;
    s.clock_accurate = accurate;
    s.conf = &g_conf;
    s.sys = &si;
    s.nickname = nickname;
    s.app_version = NULL;
    s.agent_version = AGENT_VERSION;
    s.read_only = !may_write();
    s.status = &st;
    s.timer = &pt;
    s.spent_ok = R_SUCCEEDED(sample.session_rc) && R_SUCCEEDED(sample.spent_rc);
    s.spent_ns = sample.spent_ns;
    s.records = &g_records;
    s.activity_ok = g_activity_ok;
    s.activity_s = g_activity_s;
    // What is in front, from the newest events (the totals above are read
    // every 5 minutes, this with every state: within poll_s).
    {
        u64 playing = 0, since = 0, waiting = 0;
        load_titles();   // titles.txt: what each title is
        const char *front_was = phase("the play log's newest events (pdm)");
        g_front_ok = R_SUCCEEDED(playstats_front(&playing, &since, &waiting));
        if (g_front_ok) {
            g_playing = playing;
            g_playing_since = since;
            g_waiting = waiting;
        }
        phase(front_was);
    }
    s.now_playing = g_playing;
    s.now_playing_since = g_playing_since;
    s.now_playing_kind = sync_titles_kind(&g_titles, g_playing);
    // Nothing in front: the HOME menu (or an applet), a game waiting or not.
    s.now = !(g_front_ok || g_activity_ok) ? SyncNow_Unknown : g_waiting ? SyncNow_Suspended : SyncNow_Home;
    char playing_name[129];
    name_of(g_playing, playing_name, sizeof(playing_name));
    s.now_playing_name = playing_name[0] ? playing_name : NULL;
    s.agent = true;
    s.last_result = ls->last_result;
    static char profiles[16][SYNC_PROFILE_MAX];
    s.n_profiles = host_profiles(NULL, profiles, 16);
    s.profiles = profiles;
    // The profile in effect: the saved one whose seven limits are the timer's.
    for (size_t i = 0; i < s.n_profiles && pt.fw_supported && pt.valid && !s.profile_now; i++) {
        uint16_t days[7];
        if (profile_days(NULL, profiles[i], days) && !memcmp(days, pt.day_min, sizeof(days))) s.profile_now = profiles[i];
    }
    // What changed since the last snapshot (a game started, a few minutes
    // left, games blocked): events, published after this state.
    static SyncWatch watch;
    SyncNotice notices[SYNC_NOTICE_MAX];
    const size_t n_notices = sync_watch_step(&watch, &s, notices);
    sync_engine_notices(&g_engine, notices, n_notices);
    return sync_state_build(&s, out, cap);
}

// ---- the engine's host ----

static size_t host_state(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    // PlayGuard in front: its document (it is the one reading pctl now).
    mutexLock(&g_lock);
    const bool front = g_shared.app_session && g_shared.app_foreground;
    size_t n = 0;
    if (front && g_shared.state_info.len && g_shared.state_info.len < cap) {
        n = g_shared.state_info.len;
        memcpy(out, g_shared.state, n);
    }
    mutexUnlock(&g_lock);
    if (front) return n;
    return own_state(out, cap);
}

static size_t host_activity(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    mutexLock(&g_lock);
    const bool session = g_shared.app_session;
    size_t n = 0;
    if (session && g_shared.activity_info.len && g_shared.activity_info.len < cap) {
        n = g_shared.activity_info.len;
        memcpy(out, g_shared.activity, n);
    }
    mutexUnlock(&g_lock);
    if (session) return n;
    read_activity(bl_now_ms(), false);
    if (!g_activity_len || g_activity_len >= cap) return 0;
    memcpy(out, g_activity, g_activity_len);
    return g_activity_len;
}

static bool profile_days(void *ctx, const char *name, uint16_t days[7])
{
    (void)ctx;
    const size_t n = read_file(PROFILES, g_text, sizeof(g_text));
    for (size_t at = 0; at < n;) {
        const char *line = g_text + at;
        const char *end = memchr(line, '\n', n - at);
        const size_t len = end ? (size_t)(end - line) : n - at;
        at += len + 1;
        const char *eq = memchr(line, '=', len);
        if (!eq || line[0] == '#') continue;
        size_t name_len = len - (size_t)(eq + 1 - line);
        if (name_len && eq[name_len] == '\r') name_len--;
        if (name_len != strlen(name) || memcmp(eq + 1, name, name_len)) continue;
        unsigned v[7];
        if (sscanf(line, "%u,%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) return false;
        for (int d = 0; d < 7; d++) days[d] = (uint16_t)v[d];
        return true;
    }
    return false;
}

static void append_event(const char *entity, const char *payload, const SyncOutcome *o)
{
    // One line per order the agent carried out itself, for PlayGuard's
    // history: tab-separated (agent_events.log, docs/sync-protocol.md).
    char line[400];
    u64 now = 0;
    time_local_now(&now, NULL);
    int at = snprintf(line, sizeof(line), "%llu\t%s\t%s\t%d\t%s\t%d\t%d\t%s\t", (unsigned long long)now, entity,
                      payload, o->applied ? 1 : 0, sync_reason_name(o->reason), (int)o->change, o->n,
                      o->source ? o->source : "remote");
    for (int i = 0; i < o->n && at > 0 && at < (int)sizeof(line); i++)
        at += snprintf(line + at, sizeof(line) - (size_t)at, i ? ",%d" : "%d", o->before[i]);
    if (at > 0 && at < (int)sizeof(line)) at += snprintf(line + at, sizeof(line) - (size_t)at, "\t");
    for (int i = 0; i < o->n && at > 0 && at < (int)sizeof(line); i++)
        at += snprintf(line + at, sizeof(line) - (size_t)at, i ? ",%d" : "%d", o->after[i]);
    if (at > 0 && at < (int)sizeof(line)) at += snprintf(line + at, sizeof(line) - (size_t)at, "\t%d", o->console_lock_after);
    if (at <= 0 || at >= (int)sizeof(line) - 1) return;
    line[at++] = '\n';
    FILE *f = open_write(AGENT_EVENTS, true);
    if (!f) return;
    fwrite(line, 1, (size_t)at, f);
    fclose(f);
}

// The records on the SD card before the console changes (sync_exec's
// save_records): agent_state.txt.
static bool save_records_first(void *ctx, const SyncRecords *r)
{
    (void)ctx;
    char out[512];
    const size_t n = sync_records_write(r, out, sizeof(out));
    return n && write_file(AGENT_STATE, out, n);
}

static void exec_order(const SyncIntent *in, const char *entity, const char *payload, SyncOutcome *out)
{
    char today[11];
    int weekday = -1;
    today_text(today, &weekday);
    SyncExecCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.remote_timer_writes = g_conf.remote_timer_writes;
    ctx.weekday = weekday;
    ctx.today = today;
    ctx.rec = &g_records;
    ctx.profile_days = profile_days;
    ctx.save_records = save_records_first;
    core_set_read_only(!may_write());
    const char *was = phase("an order (pctl)");
    sync_exec(in, &ctx, out);
    phase(was);
    if (out->records_changed) save_records();
    if (out->applied && out->changed) append_event(entity, payload, out);
}

static bool host_order(void *ctx, uint32_t id, const SyncIntent *intent, const char *entity, const char *payload,
                       bool retained, SyncOutcome *out)
{
    (void)ctx;
    mutexLock(&g_lock);
    const bool session = g_shared.app_session;
    bool handed = false;
    if (session) {
        AgentOrder o;
        memset(&o, 0, sizeof(o));
        o.id = id;
        o.retained = retained;
        snprintf(o.entity, sizeof(o.entity), "%s", entity);
        snprintf(o.payload, sizeof(o.payload), "%s", payload);
        o.intent = *intent;
        handed = agent_forward(&g_shared, &o);
    }
    mutexUnlock(&g_lock);
    if (session) {
        if (handed) return false;   // PlayGuard answers it (OrderResult)
        out->reason = SyncReason_Busy;
        return true;
    }
    // On its own the agent only carries out orders under "auto"; with "ask"
    // they wait (retained) for PlayGuard to confirm them on the console.
    if (g_conf.policy != SyncPolicy_Auto) {
        out->reason = SyncReason_Waiting;
        return true;
    }
    exec_order(intent, entity, payload, out);
    slog("order %s: %s", entity, out->applied ? "applied" : sync_reason_name(out->reason));
    return true;
}

static size_t host_report(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    int at = snprintf(out, cap, "=== Boreale Link report ===\nagent version: %s\nfirmware: %s\n\n", AGENT_VERSION,
                      g_firmware);
    if (at < 0 || (size_t)at >= cap) return 0;
    const char *was = phase("a report (time, pctl)");
    time_clock_dump(out + at, cap - (size_t)at);
    at += (int)strlen(out + at);
    if ((size_t)at < cap - 1) pctl_dump(out + at, cap - (size_t)at);
    phase(was);
    const size_t n = strlen(out);
    // Kept on the SD card as PlayGuard's reports are, named as they are
    // (util/log_files.hpp: report_YYYYMMDD_HHMMSS[_n].txt, local time), so
    // About › Help & diagnostics lists, sends and prunes it with them.
    mkdir(DATA_DIR "/logs", 0777);
    mkdir(REPORTS_DIR, 0777);
    u64 now = 0;
    time_local_now(&now, NULL);
    LocalTime lt;
    const TimeRule *rule = time_console_rule();
    if (!rule->to_local(rule->ctx, now, &lt)) memset(&lt, 0, sizeof(lt));
    char stamp[24], path[128];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", lt.year ? lt.year : 1970, lt.month ? lt.month : 1,
             lt.day ? lt.day : 1, lt.hour, lt.minute, lt.second);
    snprintf(path, sizeof(path), REPORTS_DIR "/report_%s.txt", stamp);
    for (unsigned k = 2; exists(path) && k < 100; k++)
        snprintf(path, sizeof(path), REPORTS_DIR "/report_%s_%u.txt", stamp, k);
    write_file(path, out, n);
    return n;
}

static size_t host_profiles(void *ctx, char (*names)[SYNC_PROFILE_MAX], size_t max)
{
    (void)ctx;
    const size_t n = read_file(PROFILES, g_text, sizeof(g_text));
    size_t count = 0;
    for (size_t at = 0; at < n && count < max;) {
        const char *line = g_text + at;
        const char *end = memchr(line, '\n', n - at);
        const size_t len = end ? (size_t)(end - line) : n - at;
        at += len + 1;
        const char *eq = memchr(line, '=', len);
        if (!eq || line[0] == '#') continue;
        size_t name_len = len - (size_t)(eq + 1 - line);
        if (name_len && eq[name_len] == '\r') name_len--;
        if (!name_len || name_len >= SYNC_PROFILE_MAX) continue;
        memcpy(names[count], eq + 1, name_len);
        names[count][name_len] = '\0';
        count++;
    }
    return count;
}

static void host_conf_changed(void *ctx, const SyncConf *conf)
{
    (void)ctx;
    // Home Assistant's discovery switch, and the old tree cleared
    // (legacy_prefix), change this way.
    g_conf.ha_discovery = conf->ha_discovery;
    memcpy(g_conf.legacy_prefix, conf->legacy_prefix, sizeof(g_conf.legacy_prefix));
    // A newer version's file, or one whose unknown lines did not all fit:
    // the switch holds until the agent restarts, the file is left as it is.
    if (sync_conf_unwritable(&g_conf)) return;
    const size_t n = sync_conf_write(&g_conf, g_text, sizeof(g_text));
    if (n && write_file(CONF_PATH, g_text, n)) g_conf_mtime = mtime_of(CONF_PATH);
}

static bool host_read_only(void *ctx)
{
    (void)ctx;
    return !may_write();
}

static uint64_t host_now_posix(void *ctx)
{
    (void)ctx;
    u64 now = 0;
    time_local_now(&now, NULL);
    return now;
}

static void host_log(void *ctx, const char *line)
{
    (void)ctx;
    slog("%s%s", line, NULL);
}

// The system document: the console read now, the space left and hekate every
// 5 minutes, PlayGuard's version as its last session said it, the serial
// number only when sync.conf's publish_serial asks for it.
#define SPACE_EVERY_MS (5 * 60 * 1000ULL)
static char g_app_version[AGENT_VERSION_MAX];
static SyncDevice g_device;   // the console as the last system document read it

static size_t host_system(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    static SysInfo si;
    static ConsoleLive live;
    static ConsoleSpace sd = { -1, -1 }, nand = { -1, -1 };
    static char hekate[16];
    static uint64_t space_at;
    sysinfo_get(&si);
    const char *was = phase("the console's readings (psm, ts, apm, nifm)");
    console_live_read(&live);
    const uint64_t now_ms = bl_now_ms();
    if (!space_at || now_ms - space_at >= SPACE_EVERY_MS) {
        space_at = now_ms ? now_ms : 1;
        phase("the storage (fs)");
        console_space_read(&sd, &nand);
        if (!console_hekate("sdmc:/", hekate)) hekate[0] = '\0';
    }
    phase(was);
    sync_device_of(&si, &live, &g_device);
    mutexLock(&g_lock);
    if (g_shared.app_session && g_shared.app_version[0])
        snprintf(g_app_version, sizeof(g_app_version), "%s", g_shared.app_version);
    mutexUnlock(&g_lock);
    u64 now = 0;
    time_local_now(&now, NULL);
    const struct mallinfo mi = mallinfo();
    const SyncAgentHealth health = { (uint32_t)(bl_now_ms() / 1000), (int32_t)mi.uordblks, INNER_HEAP_SIZE,
                                     BL_RELAY_PROTOCOL, AGENT_PROTOCOL, g_module_on };
    SyncSystem s = { now, &si, &live, &sd, &nand, hekate, g_app_version[0] ? g_app_version : NULL, AGENT_VERSION,
                     g_conf.publish_serial && g_nro.serial[0] ? g_nro.serial : NULL, &g_conf, &health };
    return sync_system_build(&s, out, cap);
}

// The console for the discovery's device: as the last system document read
// it, or read now when the discovery comes first (at the first connection).
static void host_device(void *ctx, SyncDevice *out)
{
    (void)ctx;
    if (!g_device.hos_version) {
        SysInfo si;
        ConsoleLive live;
        sysinfo_get(&si);
        const char *was = phase("the console's readings (psm, ts, apm, nifm)");
        console_live_read(&live);
        phase(was);
        sync_device_of(&si, &live, &g_device);
    }
    *out = g_device;
}

// The Home Assistant integration's document, kept as it came for PlayGuard
// (sync_files::integration_file()): it reads it when it opens and warns of a
// newer release of the integration. Cleared: the file goes. It comes again
// at each connection: the same one is not written again.
static void host_integration(void *ctx, const char *doc, size_t len)
{
    (void)ctx;
    if (len == 0) {
        remove(INTEGRATION);
        return;
    }
    char kept[SYNC_INTEGRATION_MAX + 1];
    if (read_file(INTEGRATION, kept, sizeof(kept)) == len && !memcmp(kept, doc, len)) return;
    write_file(INTEGRATION, doc, len);
}

// Every Boréale homebrew's shared bell, from their files on the SD card.
static size_t host_notices(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    u64 now = 0;
    time_local_now(&now, NULL);
    return sync_notices_build("sdmc:/", (int64_t)now, out, cap);
}

// An order for another homebrew's module (the broker brought it): handed to
// the session of that module, which fetches it by bl:link.
static void host_relay(void *ctx, const char *rel, const void *payload, size_t len, bool retained)
{
    (void)ctx;
    mutexLock(&g_lock);
    agent_relay_deliver(&g_relay, rel, payload, len, retained);
    mutexUnlock(&g_lock);
}

static const SyncHost HOST = {
    NULL, host_state, host_activity, host_order, host_report, host_profiles, host_conf_changed,
    host_read_only, host_now_posix, host_log, host_integration, host_system, host_device, host_notices,
    host_relay,
};


// ---- the loop ----

static bool sockets_up(void)
{
    if (g_sockets) return true;
    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x2000,
        .tcp_rx_buf_size = 0x2000,
        .tcp_tx_buf_max_size = 0x8000,
        .tcp_rx_buf_max_size = 0x8000,
        .udp_tx_buf_size = 0,
        .udp_rx_buf_size = 0,
        // The network service's room for this process's socket buffers, taken
        // from the heap: four times their sizes, libnx's own default. Seen
        // on a console: the loop stopped twice right after the week, the
        // next document being the games (64 KiB). Room of the buffers' own
        // size (one time) can be filled by it, and then the ACKs that would
        // empty it have none.
        .sb_efficiency = 4,
        .num_bsd_sessions = 2,   // the watchdog's shutdown() does not wait behind a call that hangs
        .bsd_service_type = BsdServiceType_Auto,
    };
    g_sockets = R_SUCCEEDED(socketInitialize(&cfg));
    return g_sockets;
}

static void engine_start(void)
{
    g_tcp.fd = -1;
    memset(&g_tls, 0, sizeof(g_tls));
    g_tls.tcp.fd = -1;
    const BlIo io = g_conf.tls ? bl_tls_io(&g_tls, g_conf.ca_file) : bl_tcp_io(&g_tcp);
    sync_engine_init(&g_engine, &g_conf, io, &HOST, bl_now_ms, "agent", AGENT_VERSION);
    sync_engine_set_module(&g_engine, g_module_on);
    g_engine_on = true;
    mutexLock(&g_lock);
    sync_conf_device_id(&g_conf, g_shared.console_id);   // this system's
    mutexUnlock(&g_lock);
    slog("link started (%.64s)%s", g_conf.host, NULL);
}

static void engine_stop(void)
{
    if (!g_engine_on) return;
    sync_engine_stop(&g_engine);
    g_engine_on = false;
}

// The loop not turned for a minute: said once in the log, with where it
// stopped, and again when it turns. Stopped in the network, the socket is
// shut down from here (the call then returns, and the link connects again);
// in a service, nothing can free it from outside, and the log says which.
#define WATCHDOG_MS 60000ULL
static Thread g_watchdog;

// The watchdog's lines, kept on the SD card too: a console restarted to free
// the link loses the log above (PlayGuard's report shows both). Started again
// past 16 KiB. When the card is what hangs, this waits, after the log above.
static void watchdog_keep(const char *line)
{
    char stamp[32];
    if (!wall_stamp(stamp)) snprintf(stamp, sizeof(stamp), "(time unknown)");
    struct stat st;
    const bool full = stat(AGENT_WATCHDOG, &st) == 0 && st.st_size > 16 * 1024;
    FILE *f = open_write(AGENT_WATCHDOG, !full);
    if (!f) return;
    fprintf(f, "%s %s\n", stamp, line);
    fclose(f);
}

static void watchdog(void *arg)
{
    (void)arg;
    uint64_t told = 0, prev = 0;
    for (;;) {
        svcSleepThread(5000000000ULL);
        const uint64_t turn = g_turn_ms, now = bl_now_ms();
        // A round long after the last one (the console slept, and the tick
        // may have run on): the loop has not had its chance to turn yet.
        const bool awake = prev && now - prev < 15000;
        prev = now;
        if (told && turn != told) {
            told = 0;
            slog("the loop turns again%s%s", NULL, NULL);
            watchdog_keep("the loop turns again");
        }
        if (!awake || !turn || now < turn || now - turn < WATCHDOG_MS || told == turn) continue;
        told = turn;
        const char *where = g_phase;
        bool freed = false;
        if (where == PHASE_LINK || where == PHASE_HANDOVER || where == PHASE_ICON) {
            const int fd = g_conf.tls ? g_tls.tcp.fd : g_tcp.fd;
            freed = fd >= 0 && shutdown(fd, SHUT_RDWR) == 0;
        }
        char line[AGENT_LOG_LINE];
        snprintf(line, sizeof(line), "the loop has not turned for %llu s, stopped in %s%s",
                 (unsigned long long)((now - turn) / 1000), where, freed ? ": its socket shut down to free it" : "");
        slog("%s%s", line, NULL);
        watchdog_keep(line);
    }
}

static void reload(bool force)
{
    if (!s_sd_mounted) {   // not there at boot: tried again here
        if (!wait_for_sd(1)) return;
        make_dirs();
        force = true;
    }
    const bool conf_changed = force || mtime_of(CONF_PATH) != g_conf_mtime || mtime_of(LINK_PATH) != g_link_mtime;
    const SyncConf before = g_conf;
    if (conf_changed) load_conf();
    if (force || mtime_of(NRO_STATE) != g_nro_mtime) {
        // PlayGuard saved (it adopts the agent's records first): its view wins.
        const size_t n = read_file(NRO_STATE, g_text, sizeof(g_text));
        agent_nro_parse(&g_nro, n ? g_text : NULL, n);
        g_nro_mtime = mtime_of(NRO_STATE);
        if (g_nro.known) {
            g_records = g_nro.records;
            save_records();
        }
    }
    module_refresh();
    if (!conf_changed || !g_engine_on) return;
    // Another stream (TLS) needs another engine; else it reconnects if needed.
    if (before.tls != g_conf.tls || strcmp(before.ca_file, g_conf.ca_file)) {
        engine_stop();
        engine_start();
    } else {
        sync_engine_reconfigure(&g_engine, &g_conf);
    }
}

// A new local day: the finished one, and extra time put back (PlayGuard does
// both itself while it runs).
static void new_day(bool app_session)
{
    char today[11];
    today_text(today, NULL);
    if (!today[0] || !strcmp(today, g_today)) return;
    const bool first = !g_today[0];
    memcpy(g_today, today, sizeof(g_today));
    if (app_session) return;
    if (!first && g_conf.publish_activity && sync_engine_online(&g_engine)) {
        read_activity(bl_now_ms(), true);
        // Yesterday, as the date it was.
        char date[11] = "";
        const TimeRule *rule = time_console_rule();
        u64 now = 0;
        time_local_now(&now, NULL);
        LocalTime t;
        if (rule && rule->to_local(rule->ctx, local_midnight(rule, now, 1), &t)) date_of(&t, date);
        if (date[0]) {
            const size_t n = final_doc(1, date, g_doc, sizeof(g_doc));
            char sub[32];
            snprintf(sub, sizeof(sub), "activity/%s", date);
            if (n) sync_engine_publish(&g_engine, sub, g_doc, n, true);
        }
    }
    if (may_write()) {
        int weekday = -1;
        today_text(today, &weekday);
        SyncExecCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.remote_timer_writes = true;   // putting the usual limit back is not a remote order
        ctx.weekday = weekday;
        ctx.today = today;
        ctx.rec = &g_records;
        core_set_read_only(false);
        SyncOutcome out;
        const char *was = phase("the usual limit put back (pctl)");
        const bool restored = sync_exec_restore_extra(&ctx, &out);
        phase(was);
        if (restored || out.records_changed) {
            if (out.records_changed) save_records();
            if (out.applied && out.changed) append_event("restore_extra", "", &out);
            slog("usual limit put back: %s%s", out.applied ? "done" : sync_reason_name(out.reason), NULL);
        }
    }
}

static void hand_over(void)
{
    // Results PlayGuard sent, its requests, and what it pushed.
    AgentResult results[AGENT_ORDERS_MAX];
    uint32_t waiting[AGENT_ORDERS_MAX];
    size_t n_waiting = 0;
    mutexLock(&g_lock);
    const size_t n_results = agent_take_results(&g_shared, results, AGENT_ORDERS_MAX);
    const bool sync_now = g_shared.want_sync_now, want_reload = g_shared.want_reload;
    const bool session_changed = g_shared.session_changed, session = g_shared.app_session;
    const bool foreground_changed = g_shared.foreground_changed;
    g_shared.want_sync_now = g_shared.want_reload = g_shared.session_changed = g_shared.foreground_changed = false;
    const bool state_new = g_shared.state_info.fresh, activity_new = g_shared.activity_info.fresh;
    g_shared.state_info.fresh = g_shared.activity_info.fresh = false;
    char app_version[AGENT_VERSION_MAX];
    snprintf(app_version, sizeof(app_version), "%s", g_shared.app_version);
    mutexUnlock(&g_lock);

    for (size_t i = 0; i < n_results; i++) {
        SyncOutcome o;
        sync_outcome_init(&o);
        o.rc = results[i].rc;
        o.applied = results[i].applied;
        o.changed = results[i].changed;
        o.reason = (SyncReason)results[i].reason;
        o.relock_failed = results[i].relock_failed;
        sync_engine_order_done(&g_engine, results[i].id, &o);
    }
    if (want_reload) {
        // PlayGuard saved: sync.conf, the profiles, read-only mode.
        reload(true);
        if (g_engine_on) sync_engine_discovery_changed(&g_engine);
    }
    if (session_changed) {
        if (session) {
            slog("PlayGuard opened (%s)%s", app_version, NULL);
        } else {
            mutexLock(&g_lock);
            n_waiting = agent_take_closed(&g_shared, waiting, AGENT_ORDERS_MAX);
            mutexUnlock(&g_lock);
            for (size_t i = 0; i < n_waiting; i++) {
                SyncOutcome o;
                sync_outcome_init(&o);
                o.reason = SyncReason_Waiting;
                sync_engine_order_done(&g_engine, waiting[i], &o);
            }
            reload(true);   // what PlayGuard saved last
            slog("PlayGuard closed%s%s", NULL, NULL);
        }
        // The orders kept on the broker come again: to PlayGuard now that
        // it is there, or to the agent now that it is gone.
        sync_engine_replay_orders(&g_engine);
        sync_engine_state_changed(&g_engine);
        sync_engine_activity_changed(&g_engine);
    }
    if (foreground_changed || state_new) sync_engine_state_changed(&g_engine);
    if (activity_new) sync_engine_activity_changed(&g_engine);
    if (sync_now) sync_engine_sync_now(&g_engine);

    // Names, the week and finished days, published as they come.
    if (!sync_engine_online(&g_engine)) return;
    size_t n;
    mutexLock(&g_lock);
    n = agent_take_doc(g_shared.names, &g_shared.names_info, g_doc, sizeof(g_doc));
    mutexUnlock(&g_lock);
    if (n && g_conf.publish_activity) sync_engine_publish(&g_engine, "names", g_doc, n, true);
    mutexLock(&g_lock);
    n = agent_take_doc(g_shared.week, &g_shared.week_info, g_doc, sizeof(g_doc));
    mutexUnlock(&g_lock);
    if (n && g_conf.publish_activity) sync_engine_publish(&g_engine, "week", g_doc, n, true);
    mutexLock(&g_lock);
    n = agent_take_doc(g_shared.games, &g_shared.games_info, g_doc, sizeof(g_doc));
    mutexUnlock(&g_lock);
    if (n && g_conf.publish_games) sync_engine_publish(&g_engine, "games", g_doc, n, true);
    for (int i = 0; i < AGENT_FINALS; i++) {
        char sub[32] = "";
        n = 0;
        mutexLock(&g_lock);
        AgentFinal *f = &g_shared.finals[i];
        if (f->fresh && f->len < sizeof(g_doc)) {
            snprintf(sub, sizeof(sub), "activity/%s", f->date);
            memcpy(g_doc, f->doc, f->len);
            n = f->len;
            f->fresh = false;
        }
        mutexUnlock(&g_lock);
        // An empty document clears the day (PlayGuard drops the old ones, and
        // every one when play time is no longer published).
        if (sub[0] && (g_conf.publish_activity || !n)) sync_engine_publish(&g_engine, sub, g_doc, n, true);
    }
}

// The homebrews that relay through the agent (bl:link): their modules' order
// topics are subscribed to, and what they asked to publish goes out, a few
// per turn so the loop is not held up.
static void relay_pump(void)
{
    SyncRelayModule open[AGENT_RELAY_SESSIONS];
    size_t n_open = 0;
    const bool online = sync_engine_online(&g_engine);
    for (int i = 0; i < AGENT_RELAY_SESSIONS; i++) {
        mutexLock(&g_lock);
        const bool got = agent_relay_module_at(&g_relay, i, open[n_open].name, sizeof(open[n_open].name),
                                               open[n_open].version, sizeof(open[n_open].version));
        mutexUnlock(&g_lock);
        if (got) n_open++;
    }
    sync_engine_relay_sync(&g_engine, open, n_open);
    mutexLock(&g_lock);
    agent_relay_set_link(&g_relay, g_shared.console_id, online);
    mutexUnlock(&g_lock);
    if (!online) return;
    for (int n = 0; n < 4; n++) {
        char topic[BL_APP_MODULE_MAX + BL_RELAY_SUB_MAX];
        size_t len = 0;
        bool retained = false;
        mutexLock(&g_lock);
        const bool got = agent_relay_take_publish(&g_relay, topic, sizeof(topic), g_doc, sizeof(g_doc), &len, &retained);
        mutexUnlock(&g_lock);
        if (!got) break;
        sync_engine_publish_relay(&g_engine, topic, g_doc, len, retained);
    }
}

static void publish_status(void)
{
    mutexLock(&g_lock);
    g_shared.status.link = *sync_engine_status(&g_engine);
    g_shared.status.pending = (uint32_t)sync_engine_pending(&g_engine);
    g_shared.status.configured = g_conf.enabled && !sync_conf_problem(&g_conf);
    g_shared.status.read_only = !may_write();
    g_shared.status.uptime_ms = bl_now_ms();
    mutexUnlock(&g_lock);
}

// ---- the icon of the game played ----

static u64 g_icon_looked;   // the game whose icon was looked for last (0: none)

static size_t read_icon(void *ctx, void *buf, size_t cap)
{
    return fread(buf, 1, cap, (FILE *)ctx);
}

// The game played has no icon on the broker yet (PlayGuard publishes them,
// docs/sync-protocol.md: one it has not sent, its link offline then, or
// publish_icons turned on since): its JPEG from PlayGuard's icon cache on the
// SD card, streamed a piece at a time (the engine's scratch: no buffer of its
// own), then added to agent_icons.txt, which PlayGuard takes into its
// icons.txt. A game PlayGuard never read (installed or first played since it
// last ran) is not in the cache: without ns the agent cannot read its
// control data, PlayGuard publishes it next time. Looked for once per game.
static void publish_playing_icon(void)
{
    const u64 id = g_playing;
    if (!id || id == g_icon_looked || !g_conf.publish_icons || !sync_engine_online(&g_engine)) return;
    if (sync_titles_kind(&g_titles, id) != SyncTitle_Game) return;   // only games' icons
    g_icon_looked = id;
    char target[SYNC_ICONS_TARGET_LEN + 1];
    sync_icons_target(g_conf.host, g_conf.port, g_engine.base, target);
    size_t n = read_file(ICONS, g_text, sizeof(g_text));
    if (sync_icons_listed(g_text, n, target, id)) return;
    n = read_file(AGENT_ICONS, g_text, sizeof(g_text));
    if (sync_icons_listed(g_text, n, target, id)) return;
    char path[96], sub[32], id_text[17];
    snprintf(id_text, sizeof(id_text), "%016llX", (unsigned long long)id);
    snprintf(path, sizeof(path), ICON_CACHE "/%s.jpg", id_text);
    snprintf(sub, sizeof(sub), "icon/%s", id_text);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 4 || st.st_size > PLAYSTATS_ICON_MAX) return;   // PlayGuard's to send
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char head[2];
    const bool jpeg = fread(head, 1, 2, f) == 2 && head[0] == 0xFF && head[1] == 0xD8 && fseek(f, 0, SEEK_SET) == 0;
    const int rc = jpeg ? sync_engine_publish_stream(&g_engine, sub, (size_t)st.st_size, true, read_icon, f) : -1;
    fclose(f);
    if (rc < 0) {
        // The connection lost on the way: again once it is back.
        if (jpeg && !sync_engine_online(&g_engine)) g_icon_looked = 0;
        return;
    }
    n = read_file(AGENT_ICONS, g_text, sizeof(g_text));
    n = sync_icons_add(g_text, n, sizeof(g_text), target, id, (uint32_t)st.st_size);
    if (n) write_file(AGENT_ICONS, g_text, n);
    slog("icon of %s published from PlayGuard's cache%s", id_text, NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mutexInit(&g_lock);
    agent_shared_init(&g_shared, AGENT_VERSION);
    agent_relay_init(&g_relay);
    agent_relay_set_version(&g_relay, AGENT_VERSION);
    {
        char fw[16];
        sysinfo_version_string(hosversionGet(), fw, sizeof(fw));
        snprintf(g_firmware, sizeof(g_firmware), "%s", fw);
    }
    system_paths();
    wait_for_sd(40);
    make_dirs();
    load_conf();
    load_records();
    g_module_on = module_read();
    agent_ipc_start();
    wall_read(bl_now_ms());
    if (R_SUCCEEDED(threadCreate(&g_watchdog, watchdog, NULL, NULL, 0x2000, 0x2D, -2))) threadStart(&g_watchdog);
    slog("agent %s started, firmware %s", AGENT_VERSION, g_firmware);

    for (;;) {
        const uint64_t now = bl_now_ms();
        g_turn_ms = now;
        phase("the clock (time)");
        wall_read(now);
        phase("the loop");
        if (now - g_conf_checked >= CONF_CHECK_MS) {
            g_conf_checked = now;
            phase("sync.conf (SD card)");
            reload(false);
        }
        mutexLock(&g_lock);
        // A manager asked it to leave (bl:link's Shutdown), for an update or a stop.
        const bool shutdown = agent_relay_leaving(&g_relay);
        const bool session = g_shared.app_session;
        mutexUnlock(&g_lock);
        if (shutdown && !g_stopped) {
            engine_stop();   // "offline", DISCONNECT
            g_stopped = true;
            slog("stopped: asked to leave the broker%s%s", NULL, NULL);
        } else if (!shutdown && g_stopped) {
            // PlayGuard said Hello again: the update or the stop did not happen.
            g_stopped = false;
            slog("resumed: back on the broker%s%s", NULL, NULL);
        }
        if (g_stopped || !g_conf_ok || !g_conf.enabled || sync_conf_problem(&g_conf)) {
            phase(PHASE_LINK);
            if (!g_stopped) engine_stop();
            publish_status();
            phase("waiting (link off)");
            svcSleepThread(1000000000ULL);
            // Requests still come (reload, a new session); results have
            // nowhere to go and wait.
            mutexLock(&g_lock);
            if (g_shared.want_reload) {
                g_shared.want_reload = false;
                mutexUnlock(&g_lock);
                reload(true);
            } else {
                mutexUnlock(&g_lock);
            }
            continue;
        }
        phase("the network's start (bsd)");
        if (!sockets_up()) {
            svcSleepThread(5000000000ULL);
            continue;
        }
        if (!g_engine_on) engine_start();
        if (g_module_on) {
            phase("the new day");
            new_day(session);
            phase(PHASE_HANDOVER);
            hand_over();
        }
        relay_pump();
        phase(PHASE_LINK);
        const int sleep_ms = sync_engine_step(&g_engine, 500);
        phase(PHASE_ICON);
        if (g_module_on) publish_playing_icon();
        publish_status();
        phase("waiting");
        if (sleep_ms > 0) svcSleepThread((u64)(sleep_ms > 1000 ? 1000 : sleep_ms) * 1000000ULL);
    }
    return 0;
}
