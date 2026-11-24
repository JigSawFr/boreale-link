// The console's readers of console_info.h (libnx): PlayGuard and the agent.
// Each service is opened for its read and closed again, so the agent keeps
// no session it does not need between two documents; a failed call leaves
// its field unknown.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "console_info.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

static void nickname_of(char out[0x80])
{
    SetSysDeviceNickName nick;
    memset(&nick, 0, sizeof(nick));
    out[0] = '\0';
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick))) {
        _Static_assert(sizeof(((ConsoleLive *)0)->nickname) == sizeof(nick.nickname), "the nickname's size");
        memcpy(out, nick.nickname, 0x80 - 1);
        out[0x80 - 1] = '\0';
    }
}

// ---- what a read is doing, and how long it took ----
// Each call to a service of the console can block (psm, nifm, ts and the rest
// answer late around a sleep): the stage under way is kept where another
// thread can see it (the agent's watchdog names it while it hangs), and a
// call longer than the threshold is reported once it returns.
static console_slow_fn g_slow;
static uint32_t g_slow_ms = 1000;
static const char *volatile g_stage;
static volatile u64 g_stage_tick;

void console_info_watch(console_slow_fn fn, uint32_t slow_ms)
{
    g_slow = fn;
    g_slow_ms = slow_ms;
}

const char *console_read_stage(uint64_t *since_ms)
{
    const char *what = g_stage;
    const u64 tick = g_stage_tick;
    if (!what) return NULL;
    if (since_ms) *since_ms = armTicksToNs(armGetSystemTick() - tick) / 1000000ULL;
    return what;
}

static void stage_begin(const char *what)
{
    g_stage_tick = armGetSystemTick();
    g_stage = what;
}

static void stage_end(void)
{
    const char *what = g_stage;
    const u64 ms = armTicksToNs(armGetSystemTick() - g_stage_tick) / 1000000ULL;
    g_stage = NULL;
    if (what && g_slow && ms >= g_slow_ms) g_slow(what, (uint32_t)ms);
}

static void read_setsys(ConsoleLive *out)
{
    stage_begin("set:sys (model, nickname)");
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysProductModel model = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysGetProductModel(&model))) out->model = console_model_of((int)model);
        nickname_of(out->nickname);
        setsysExit();
    }
    stage_end();
}

void console_nickname_read(char out[0x80])
{
    out[0] = '\0';
    stage_begin("set:sys (nickname)");
    if (R_SUCCEEDED(setsysInitialize())) {
        nickname_of(out);
        setsysExit();
    }
    stage_end();
}

static void read_region(ConsoleLive *out)
{
    stage_begin("set (region)");
    if (R_SUCCEEDED(setInitialize())) {
        SetRegion region;
        const char *name = R_SUCCEEDED(setGetRegionCode(&region)) ? console_region_name((int)region) : NULL;
        if (name) snprintf(out->region, sizeof(out->region), "%s", name);
        setExit();
    }
    stage_end();
}

static void read_battery(ConsoleLive *out)
{
    stage_begin("psm (battery, charger)");
    if (R_SUCCEEDED(psmInitialize())) {
        u32 pct = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct)) && pct <= 100) out->battery_pct = (int)pct;
        PsmChargerType charger;
        if (R_SUCCEEDED(psmGetChargerType(&charger)) && (int)charger >= 0 && (int)charger <= 3)
            out->charger = (int)charger;
        psmExit();
    }
    stage_end();
}

static void read_dock(ConsoleLive *out)
{
    // Docked: the operation mode omm keeps (what the dock says, the console
    // asleep in it too; the applet's is not there for a sysmodule), else the
    // boost performance mode (docked and awake only).
    stage_begin("omm (operation mode)");
    Service omm;
    if (R_SUCCEEDED(smGetService(&omm, "omm"))) {
        u8 mode = 0xFF;
        if (R_SUCCEEDED(serviceDispatchOut(&omm, 0, mode)) && mode <= 1) out->docked = mode;   // GetOperationMode: 1 console
        serviceClose(&omm);
    }
    stage_end();
    if (out->docked >= 0) return;
    stage_begin("apm (performance mode)");
    if (R_SUCCEEDED(apmInitialize())) {
        ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
        if (R_SUCCEEDED(apmGetPerformanceMode(&mode)) && mode != ApmPerformanceMode_Invalid)
            out->docked = mode == ApmPerformanceMode_Boost;
        apmExit();
    }
    stage_end();
}

static void read_temperature(ConsoleLive *out)
{
    stage_begin("ts (temperature)");
    if (R_SUCCEEDED(tsInitialize())) {
        TsSession s;
        if (R_SUCCEEDED(tsOpenSession(&s, TsDeviceCode_LocationInternal))) {
            float t = 0;
            if (R_SUCCEEDED(tsSessionGetTemperature(&s, &t)) && t > -50 && t < 150) out->temperature_c = (int)(t + 0.5f);
            tsSessionClose(&s);
        }
        tsExit();
    }
    stage_end();
}

static void read_network(ConsoleLive *out)
{
    stage_begin("nifm (network)");
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
        stage_end();
        return;
    }
    NifmInternetConnectionType type;
    NifmInternetConnectionStatus status;
    u32 strength = 0;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
        out->network = type == NifmInternetConnectionType_WiFi ? 1 : type == NifmInternetConnectionType_Ethernet ? 2 : 0;
        if (out->network == 1 && strength <= 3) out->wifi_bars = (int)strength;
    } else {
        out->network = 0;   // no connection at all
    }
    u32 addr = 0;
    if (out->network > 0 && R_SUCCEEDED(nifmGetCurrentIpAddress(&addr)) && addr) console_ip_text(addr, out->ip);
    if (out->network == 1) {
        // The profile holds the passphrase too: only the SSID is kept, and
        // the copy is wiped (static: off the agent's small stack).
        static NifmNetworkProfileData p;
        memset(&p, 0, sizeof(p));
        if (R_SUCCEEDED(nifmGetCurrentNetworkProfile(&p))) {
            size_t n = p.wireless_setting_data.ssid_len;
            if (n > sizeof(out->ssid) - 1) n = sizeof(out->ssid) - 1;
            memcpy(out->ssid, p.wireless_setting_data.ssid, n);
            out->ssid[n] = '\0';
        }
        volatile uint8_t *w = (volatile uint8_t *)&p;
        for (size_t i = 0; i < sizeof(p); i++) w[i] = 0;
    }
    nifmExit();
    stage_end();
}

static void uptime_read(ConsoleLive *out)
{
    const u64 freq = armGetSystemTickFreq();
    if (freq) out->uptime_s = armGetSystemTick() / freq;
}

void console_live_read_dynamic(ConsoleLive *out)
{
    // What changes while it runs, unknown again before its read; the model,
    // the nickname and the region stay as they were.
    out->battery_pct = -1;
    out->charger = -1;
    out->docked = -1;
    out->temperature_c = -1000;
    out->network = -1;
    out->ip[0] = '\0';
    out->wifi_bars = -1;
    out->ssid[0] = '\0';
    out->uptime_s = 0;
    read_battery(out);
    read_dock(out);
    read_temperature(out);
    read_network(out);
    uptime_read(out);
}

void console_live_read(ConsoleLive *out)
{
    console_live_init(out);
    read_setsys(out);
    read_region(out);
    console_live_read_dynamic(out);
}

static void space_of(FsFileSystem *fs, ConsoleSpace *out)
{
    s64 free_b = 0, total_b = 0;
    if (R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free_b))) out->free_mb = free_b / (1024 * 1024);
    if (R_SUCCEEDED(fsFsGetTotalSpace(fs, "/", &total_b))) out->total_mb = total_b / (1024 * 1024);
}

void console_space_read(ConsoleSpace *sd, ConsoleSpace *nand)
{
    sd->free_mb = sd->total_mb = nand->free_mb = nand->total_mb = -1;
    stage_begin("fs (space left)");
    FsFileSystem fs;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&fs))) {
        space_of(&fs, sd);
        fsFsClose(&fs);
    }
    if (R_SUCCEEDED(fsOpenBisFileSystem(&fs, FsBisPartitionId_User, ""))) {
        space_of(&fs, nand);
        fsFsClose(&fs);
    }
    stage_end();
}
