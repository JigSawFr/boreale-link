// PlayGuard — pctl service layer (see pctl_ops.h).
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See the
// LICENSE file or <https://www.gnu.org/licenses/gpl-3.0.html> for details.
#include "pctl_ops.h"
#include "fw_policy.h"
#include "pctl_ops_internal.h"
#include "pure.h"
#include "rescue.h"
#include "core/write_guard.h"
#include <string.h>
#include <stdio.h>

/*
 * Command IDs for IParentalControlService.
 * Reference: https://switchbrew.org/wiki/Parental_Control_services
 * What was found on hardware is gathered in docs/parental-controls.md.
 * Checked against the command table up to 23.0.1 (PCTL_FW_TABLE_CHECKED: no ID
 * used here changed between 21.0.0 and 23.0.1; 23.0.0 only *added*
 * 1023/1412/1460/2025-2027/9407). Verified on a console up to 22.5.0
 * (PCTL_FW_TESTED_MAX): newer goes through the firmware gate.
 *
 *   1006 IsRestrictionTemporaryUnlocked   -> bool
 *   1007 RevertRestrictionTemporaryUnlocked (no args)
 *   1031 IsRestrictionEnabled             -> bool
 *   1032 GetSafetyLevel                   -> u32       1033 SetSafetyLevel <- u32
 *   1035 GetCurrentSettings               -> RestrictionSettings (3 bytes)
 *   1036 SetCustomSafetyLevelSettings     <- RestrictionSettings (3 bytes)
 *   1034 GetSafetyLevelSettings          <- u32 level -> RestrictionSettings (3 bytes)
 *   1037 GetDefaultRatingOrganization     -> u32       1038 SetDefaultRatingOrganization <- u32
 *   1039 GetFreeCommunicationApplicationListCount -> u32
 *   1044 GetFreeCommunicationApplicationList <- u32 offset -> u32 count + Out|MapAlias buffer
 *        (FreeCommunicationApplicationInfo, 16 bytes: the application ID, then 8 bytes not understood)
 *   1043 DeleteSettings                   (no args) privileged -- IRREVERSIBLE
 *   1903 GetExemptApplicationListCountForDebug [5.0.0+] -> u32, and
 *   1904 GetExemptApplicationListForDebug [5.0.0+], read like 1039 / 1044
 *        (no published signature: taken to mirror them; dumped raw, 1904
 *        only when 1903 counts an entry)
 *   Not called: 9401/9402 GetEvents(WithJson) [20.0.0+] and 1404/1411/1421
 *   (paired account): no published signature, and 1404 may go online.
 *   1062 GetStereoVisionRestriction       -> bool [4.0.0+]   1063 Set… <- bool
 *   1201 UnlockRestrictionTemporarily     <- PIN, In|HipcPointer buffer, NUL-terminated
 *                                            (MapAlias or no buffer => 0xF601 session closed;
 *                                             digits without '\0' => 0xF80E). Verified fw 22.1.0.
 *   1206 GetPinCodeLength                 -> u32 (0 == no PIN)
 *   1208 GetPinCode [4.0.0+]              -> u32 len + Out|HipcPointer buffer
 *   1403 IsPairingActive                  -> bool
 *   1406 GetSettingsLastUpdated           -> PosixTime
 *   1451 StartPlayTimer / 1452 StopPlayTimer (no args)
 *   1453 IsPlayTimerEnabled -> bool  1454 GetPlayTimerRemainingTime -> TimeSpan(ns)
 *   1455 IsRestrictedByPlayTimer -> bool   1458 IsPlayTimerAlarmDisabled -> bool [4.0.0+]
 *   1459 GetPlayTimerRemainingTimeDisplayInfo [20.0.0+] (0x20 bytes: u8 at 0, 2 with a limit
 *        today; the remaining ns at 0x10, as 1454 — docs/parental-controls.md)
 *   1460 GetWatcherStatusDisplayInfo [23.0.0+] (in 1 byte, out 0x18 bytes, layout unknown)
 *   1941 DeletePairing (no args) privileged
 *   1952 GetPlayTimerSpentTimeForTest -> TimeSpan   1953 SetPlayTimerAlarmDisabledForDebug <- bool
 *   1954 IsBedtimeAlarmEnabled -> bool [18.0.0+]  1956/1957 GetBedtimeAlarmTimeHour/Minute [18.0.0+]
 *   1958/1959 GetBedtimeAlarmResetTimeHour/Minute [20.0.0+]  1960 GetExtraPlayingTimeForDebug [20.0.0+]
 *   145601 GetPlayTimerSettings -> 0x44 bytes [21.0.0+ size]   195101 SetPlayTimerSettingsForDebug <- 0x44
 *   Never call 1456 / 1951: they are the [18.0.0-20.5.0] "Old" variants and close the session on 21+.
 *   1601/1602/1603 return 0x00010A8E on a normal console — not usable.
 *
 * Registering / changing the PIN goes through the OS library applet
 * (pctlauthRegisterPasscode); the direct SetPinCode command is rejected.
 *
 * The privileged calls only succeed under Atmosphère, where libnx's
 * pctlInitialize lands on a `pctl:a` / `pctl:s` session.
 */

// ---------------------------------------------------------------- session

static bool s_owned_open = false;

Result pctl_ops_init(void)
{
    if (s_owned_open) return 0;
    Result rc = pctlInitialize();
    if (R_SUCCEEDED(rc)) s_owned_open = true;
    return rc;
}

void pctl_ops_exit(void)
{
    if (!s_owned_open) return;
    pctlExit();
    s_owned_open = false;
}

Result pctl_ops_reinit(void)
{
    pctl_ops_exit();
    return pctl_ops_init();
}

// Runs one no-argument command in its own session. `relock`: locking again,
// which only read-only mode refuses (never the PIN check, write_guard.h).
static Result run_simple(u32 cmd, bool relock)
{
    Result rc = relock ? (core_read_only() ? NXM_RC_READ_ONLY : 0) : core_change_allowed();
    if (R_FAILED(rc)) return rc;
    rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatch(pctlGetServiceSession_Service(), cmd);
    pctl_ops_exit();
    return rc;
}

// ---------------------------------------------------------------- status

// The fields every screen needs; status_read_rest adds the others.
static void status_read_core(Service *srv, PctlStatus *out)
{
    out->safety_level_ok        = R_SUCCEEDED(rd_u32 (srv, 1032, &out->safety_level));
    out->pin_length_ok          = R_SUCCEEDED(rd_u32 (srv, 1206, &out->pin_length));
    out->restriction_enabled_ok = R_SUCCEEDED(rd_bool(srv, 1031, &out->restriction_enabled));
    out->temp_unlocked_ok       = R_SUCCEEDED(rd_bool(srv, 1006, &out->temp_unlocked));
    out->pairing_active_ok      = R_SUCCEEDED(rd_bool(srv, 1403, &out->pairing_active));
}

static void status_read_rest(Service *srv, PctlStatus *out)
{
    out->rating_org_ok          = R_SUCCEEDED(rd_u32 (srv, 1037, &out->rating_org));
    out->free_comm_count_ok     = R_SUCCEEDED(rd_u32 (srv, 1039, &out->free_comm_count));
    out->last_updated_ok        = R_SUCCEEDED(rd_u64 (srv, 1406, &out->last_updated));

    u8 raw[3] = {0};
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1035, raw))) {
        out->settings.rating_age                     = raw[0];
        out->settings.sns_post_restriction           = raw[1] != 0;
        out->settings.free_communication_restriction = raw[2] != 0;
        out->settings_ok = true;
    }
    if (hosversionAtLeast(4, 0, 0))
        out->stereo_vision_ok = R_SUCCEEDED(rd_bool(srv, 1062, &out->stereo_vision_restricted));
}

void pctl_status_fetch(PctlStatus *out)
{
    memset(out, 0, sizeof(*out));
    out->session_rc = pctl_ops_init();
    if (R_FAILED(out->session_rc)) return;
    Service *srv = pctlGetServiceSession_Service();
    status_read_core(srv, out);
    status_read_rest(srv, out);
    pctl_ops_exit();
}

Result pctl_lock_state(u32 *pin_length, bool *unlocked)
{
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    Service *srv = pctlGetServiceSession_Service();
    if (pin_length) rc = rd_u32(srv, 1206, pin_length);
    if (R_SUCCEEDED(rc) && unlocked) rc = rd_bool(srv, 1006, unlocked);
    pctl_ops_exit();
    return rc;
}

// ---------------------------------------------------------------- PIN / unlock

// What the pctlauth applet (switchbrew's Auth Applet: its result in a 4-byte
// output storage, which libnx returns) gives when it is left without a PIN:
// B on its screen, PCTL_RC_AUTH_CANCELLED. Said as NXM_RC_CANCELLED, which
// the screens pass over in silence.
static Result auth_result(Result rc)
{
    return rc == PCTL_RC_AUTH_CANCELLED ? NXM_RC_CANCELLED : rc;
}

Result pctl_set_pin(void)
{
    // The pctlauth applet opens its own privileged session: ours must be closed.
    pctl_ops_exit();
    Result rc = core_change_allowed();
    if (R_FAILED(rc)) return rc;
    return auth_result(pctlauthRegisterPasscode());
}

Result pctl_ask_pin(void)
{
    // GetPinCodeLength first: the applet must not be shown without a PIN.
    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    u32 len = 0;
    rc = rd_u32(pctlGetServiceSession_Service(), 1206, &len);
    // The pctlauth applet opens its own privileged session: ours must be closed.
    pctl_ops_exit();
    if (R_FAILED(rc)) return rc;
    if (len == 0) return NXM_RC_NO_PIN;
    return auth_result(pctlauthShowForConfiguration());
}

// Reads the PIN (1208), hands it to `use` and wipes it: the PIN never lives
// outside this call. `use` runs only when the read succeeded, and its Result
// is returned; `len` is the length 1208 reported.
typedef Result (*PinUse)(Service *srv, const char *pin, u32 len, void *ctx);

static Result with_pin(Service *srv, PinUse use, void *ctx)
{
    char pin[PIN_BUF];
    u32 pin_len = 0;
    Result rc = read_pin(srv, pin, &pin_len);
    if (R_SUCCEEDED(rc)) rc = use(srv, pin, pin_len, ctx);
    secure_zero(pin, sizeof(pin));
    secure_zero(&pin_len, sizeof(pin_len));
    return rc;
}

// 1201 with the PIN NUL-terminated: the length reported + 1 bytes, the whole
// buffer when that length is of no use.
static Result pin_unlock(Service *srv, const char *pin, u32 len, void *ctx)
{
    (void)ctx;
    return unlock_with_pin(srv, pin, (len > 0 && len < PIN_BUF) ? ((size_t)len + 1) : PIN_BUF);
}

Result pctl_unlock_restriction_temporarily(void)
{
    pctl_ops_exit();   // nothing held while the PIN check may show its applet
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    // Two things had to be right (both learned the hard way on fw 22.1.0):
    //  - the buffers are HIPC *pointer* buffers (SfBufferAttr_HipcPointer), not
    //    map-alias — map-alias makes the sysmodule drop the session (0xF601);
    //  - the PIN is passed NUL-terminated (GetPinCodeLength + 1 bytes) — the
    //    bare digits return 0xF80E.
    // The PIN is read with GetPinCode (1208), handed straight back and wiped;
    // it never leaves this function (pctl_get_pin is the only other reader).
    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    Service *srv = pctlGetServiceSession_Service();

    rc = with_pin(srv, pin_unlock, NULL);

    if (R_SUCCEEDED(rc)) {
        bool unlocked = false;
        Result vr = rd_bool(srv, 1006, &unlocked);
        if (R_FAILED(vr)) {
            // 1201 went through but the state cannot be read back: the caller
            // treats this as a failure and will not lock again, so do it here
            // rather than leave the console unlocked without anyone knowing.
            // Should that fail too, say so: the console may still be unlocked.
            rc = R_SUCCEEDED(serviceDispatch(srv, 1007)) ? vr : NXM_RC_RELOCK_FAILED;
        } else if (!unlocked) {
            rc = NXM_RC_UNLOCK_NOT_EFFECTIVE;
        }
    }
    pctl_ops_exit();
    return rc;
}

typedef struct {
    char  *out;
    size_t size;
} PinOut;

static Result pin_copy(Service *srv, const char *pin, u32 len, void *ctx)
{
    (void)srv;
    PinOut *o = (PinOut *)ctx;
    const size_t n = rescue_pin_length(pin, PIN_BUF, len);
    // A Switch PIN is 4 to 8 digits; anything else is not shown.
    bool digits = n >= 4 && n <= 8;
    for (size_t i = 0; i < n && digits; i++) digits = pin[i] >= '0' && pin[i] <= '9';
    if (!digits) return NXM_RC_STATE_UNKNOWN;
    if (n >= o->size) return NXM_RC_INVALID_ARGUMENT;
    memcpy(o->out, pin, n);
    o->out[n] = '\0';
    return 0;
}

Result pctl_get_pin(char *out, size_t out_size)
{
    if (out && out_size) secure_zero(out, out_size);
    pctl_ops_exit();   // nothing held while the PIN check may show its applet
    Result gate = core_reveal_allowed();   // asked every time (write_guard.h)
    if (R_FAILED(gate)) return gate;
    if (!out || out_size == 0) return NXM_RC_INVALID_ARGUMENT;
    // Same session handling as the unlock above, where 1208 was validated.
    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    PinOut o = { out, out_size };
    rc = with_pin(pctlGetServiceSession_Service(), pin_copy, &o);
    pctl_ops_exit();
    return rc;
}

static Result pin_judge(Service *srv, const char *pin, u32 len, void *ctx)
{
    (void)srv;
    const size_t n = rescue_pin_length(pin, PIN_BUF, len);
    if (n == 0) return NXM_RC_NO_PIN;
    *(bool *)ctx = pin_is_weak(pin, n);
    return 0;
}

Result pctl_pin_weak(bool *weak)
{
    if (!weak) return NXM_RC_INVALID_ARGUMENT;
    *weak = false;
    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    rc = with_pin(pctlGetServiceSession_Service(), pin_judge, weak);
    pctl_ops_exit();
    return rc;
}

typedef struct {
    const void *msg;
    size_t      size;
    const u8   *secret;
    u8         *mac;
} PinMac;

static Result pin_mac(Service *srv, const char *pin, u32 len, void *ctx)
{
    (void)srv;
    const PinMac *m = (const PinMac *)ctx;
    // The key the recovery sysmodule takes (rescue_pin_length, as it does).
    const size_t n = rescue_pin_length(pin, PIN_BUF, len);
    if (n == 0) return NXM_RC_NO_PIN;
    return rescue_proof_mac(hmacSha256CalculateMac, m->secret, pin, n, m->msg, m->size, m->mac) ? 0
                                                                                                 : NXM_RC_STATE_UNKNOWN;
}

// The console's secret the recovery sysmodule reads too (rescue.h).
static Result read_secret(u8 out[RESCUE_SECRET_SIZE])
{
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) return rc;
    Uuid id;
    rc = setsysGetMiiAuthorId(&id);
    setsysExit();
    if (R_SUCCEEDED(rc)) memcpy(out, id.uuid, RESCUE_SECRET_SIZE);
    secure_zero(&id, sizeof(id));
    return rc;
}

Result pctl_pin_hmac(const void *msg, size_t size, u8 *mac)
{
    if (!mac) return NXM_RC_INVALID_ARGUMENT;
    memset(mac, 0, 32);
    if (!msg) return NXM_RC_INVALID_ARGUMENT;
    u8 secret[RESCUE_SECRET_SIZE];
    Result rc = read_secret(secret);
    if (R_SUCCEEDED(rc)) rc = pctl_ops_reinit();
    if (R_FAILED(rc)) {
        secure_zero(secret, sizeof(secret));
        return rc;
    }
    PinMac m = { msg, size, secret, mac };
    rc = with_pin(pctlGetServiceSession_Service(), pin_mac, &m);
    pctl_ops_exit();
    secure_zero(secret, sizeof(secret));
    return rc;
}

Result pctl_relock(void)                     { return run_simple(1007, true); }
Result pctl_delete_parental_controls(void)   { return run_simple(1043, false); }
Result pctl_delete_pairing(void)             { return run_simple(1941, false); }

// ---------------------------------------------------------------- restrictions

Result pctl_get_level_settings(u32 level, PctlCustomSettings *out)
{
    if (!out || level > PctlSafetyLevel_Teen) return NXM_RC_INVALID_ARGUMENT;
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    u8 raw[3] = {0};
    rc = serviceDispatchInOut(pctlGetServiceSession_Service(), 1034, level, raw);
    pctl_ops_exit();
    if (R_SUCCEEDED(rc)) {
        out->rating_age                     = raw[0];
        out->sns_post_restriction           = raw[1] != 0;
        out->free_communication_restriction = raw[2] != 0;
    }
    return rc;
}

Result pctl_set_rating_org(u32 org)
{
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    if (org >= RATING_ORG_COUNT) return NXM_RC_INVALID_ARGUMENT;
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchIn(pctlGetServiceSession_Service(), 1038, org);
    pctl_ops_exit();
    return rc;
}

Result pctl_set_safety_level(u32 level)
{
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    if (level > PctlSafetyLevel_Teen) return NXM_RC_INVALID_ARGUMENT;
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchIn(pctlGetServiceSession_Service(), 1033, level);
    pctl_ops_exit();
    return rc;
}

Result pctl_set_custom_settings(const PctlCustomSettings *s)
{
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    if (!s || s->rating_age > 21) return NXM_RC_INVALID_ARGUMENT;
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    Service *srv = pctlGetServiceSession_Service();
    u32 level = 0;
    rc = rd_u32(srv, 1032, &level);
    if (R_SUCCEEDED(rc) && level != PctlSafetyLevel_Custom) rc = NXM_RC_NOT_CUSTOM;
    if (R_SUCCEEDED(rc)) {
        u8 raw[3] = { s->rating_age, (u8)(s->sns_post_restriction ? 1 : 0),
                      (u8)(s->free_communication_restriction ? 1 : 0) };
        rc = serviceDispatchIn(srv, 1036, raw);
    }
    pctl_ops_exit();
    return rc;
}

Result pctl_set_stereo_vision_restricted(bool restricted)
{
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    if (!hosversionAtLeast(4, 0, 0)) return NXM_RC_FW_UNSUPPORTED;
    Result rc = pctl_ops_init();
    if (R_FAILED(rc)) return rc;
    u8 v = restricted ? 1 : 0;
    rc = serviceDispatchIn(pctlGetServiceSession_Service(), 1063, v);
    pctl_ops_exit();
    return rc;
}

// ---------------------------------------------------------------- play timer

// PlayTimerSettings: layout and codec in pure.h. 1454 is in nanoseconds.

// Writing while the timer counts down destabilises Atmosphère. Every play-timer
// write checks in its own session, right before writing, so no caller can skip
// the gate: NXM_RC_WRITE_GATED while the timer is enabled (1453) or restricting
// (1455) and not temporarily unlocked (1006). `active` (may be NULL) gets
// whether the timer is enabled or restricting.
static Result pt_write_gate(Service *srv, bool *active)
{
    bool enabled = false, restricted = false, unlocked = false;
    Result rc = rd_bool(srv, 1453, &enabled);
    if (R_SUCCEEDED(rc)) rc = rd_bool(srv, 1455, &restricted);
    if (R_SUCCEEDED(rc)) rc = rd_bool(srv, 1006, &unlocked);
    if (R_FAILED(rc)) return rc;
    // Temporarily unlocked, 1453 reads false even with a limit set
    // (docs/parental-controls.md), and every write while the timer counts
    // goes through that unlock: count it as active too, or a failed read of
    // the block would let a zeroed one through.
    if (active) *active = enabled || restricted || unlocked;
    return ((enabled || restricted) && !unlocked) ? NXM_RC_WRITE_GATED : 0;
}

// 195101 with no PIN registered: pctl skips the write and still returns 0
// (the 22.5.0 sysmodule, NX-Pctl-Manager's docs/pctl-reverse-engineering.md).
// Say so before writing instead of a "not applied" after it. A failed 1206
// proves nothing: the write goes ahead, its read-back still catches a skip.
static Result pt_block_write_gate(Service *srv, bool *active)
{
    Result rc = pt_write_gate(srv, active);
    if (R_FAILED(rc)) return rc;
    u32 pin_length = 0;
    return R_SUCCEEDED(rd_u32(srv, 1206, &pin_length)) && pin_length == 0 ? NXM_RC_NO_PIN : 0;
}

// A play-timer command (no argument, or the one byte `arg` points to) behind
// the change check and the gate above, in one session.
static Result pt_gated_command(u32 cmd, const u8 *arg)
{
    pctl_ops_exit();   // nothing held while the PIN check may show its applet
    Result rc = core_change_allowed();
    if (R_FAILED(rc)) return rc;
    rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    Service *srv = pctlGetServiceSession_Service();
    rc = pt_write_gate(srv, NULL);
    if (R_SUCCEEDED(rc)) rc = arg ? serviceDispatchIn(srv, cmd, *arg) : serviceDispatch(srv, cmd);
    pctl_ops_exit();
    return rc;
}

static void pt_init(PtState *out)
{
    memset(out, 0, sizeof(*out));
    for (int n = 0; n < 7; n++) out->day_min[n] = PT_DAY_NOLIMIT;
    out->fw_supported = pt_fw_supported();
}

// Everything but 1006 (the caller reads or copies it), 1458 and the bedtime
// reset time (1958/1959), which only the Play timer tab shows.
static void pt_read_core(Service *srv, PtState *out)
{
    out->enabled_rc = rd_bool(srv, 1453, &out->enabled);
    out->enabled_valid = R_SUCCEEDED(out->enabled_rc);
    out->restricted_rc = rd_bool(srv, 1455, &out->restricted);
    out->restricted_valid = R_SUCCEEDED(out->restricted_rc);
    out->remaining_rc = rd_u64(srv, 1454, (u64 *)&out->remaining_ns);   // a TimeSpan: signed
    out->remaining_valid = R_SUCCEEDED(out->remaining_rc);

    u16 c[PT_U16_COUNT];
    memset(c, 0, sizeof(c));
    out->config_rc = serviceDispatchOut(srv, 145601, c);
    if (R_SUCCEEDED(out->config_rc)) {
        memcpy(out->block, c, sizeof(out->block));
        // Not the known layout: nothing decoded from it is shown or reused
        // (the block itself is kept, as read).
        if (!pt_plausible(c)) out->config_rc = NXM_RC_PT_NOT_UNDERSTOOD;
    }
    if (R_SUCCEEDED(out->config_rc)) {
        out->valid = true;
        pt_decode(c, out->day_min);
        pt_bedtime_decode(c, out->bed);
        out->alarm_only = !pt_suspends(c);
    }

    bool be = false;
    u8 h = 0, m = 0;
    if (R_SUCCEEDED(rd_bool(srv, 1954, &be)) && R_SUCCEEDED(rd_u8(srv, 1956, &h)) &&
        R_SUCCEEDED(rd_u8(srv, 1957, &m)) && h < 24 && m < 60) {
        out->bedtime_valid = true;
        out->bedtime_enabled = be;
        out->bedtime_hour = h;
        out->bedtime_minute = m;
    }
}

static void pt_read_rest(Service *srv, PtState *out)
{
    out->alarm_disabled_valid = R_SUCCEEDED(rd_bool(srv, 1458, &out->alarm_disabled));
    u8 h = 0, m = 0;
    if (hosversionAtLeast(20, 0, 0) && R_SUCCEEDED(rd_u8(srv, 1958, &h)) &&
        R_SUCCEEDED(rd_u8(srv, 1959, &m)) && h < 24 && m < 60) {
        out->bedtime_reset_valid = true;
        out->bedtime_reset_hour = h;
        out->bedtime_reset_minute = m;
    }
}

void pctl_play_timer_query(PtState *out)
{
    pt_init(out);
    if (!out->fw_supported) return;

    out->session_rc = pctl_ops_reinit();
    if (R_FAILED(out->session_rc)) return;
    out->session_valid = true;
    Service *srv = pctlGetServiceSession_Service();
    out->temporary_unlocked_rc = rd_bool(srv, 1006, &out->temporary_unlocked);
    out->temporary_unlocked_valid = R_SUCCEEDED(out->temporary_unlocked_rc);
    pt_read_core(srv, out);
    pt_read_rest(srv, out);
    pctl_ops_exit();
}

void pctl_overview_fetch(PctlStatus *status, PtState *pt)
{
    memset(status, 0, sizeof(*status));
    pt_init(pt);
    status->session_rc = pctl_ops_reinit();
    if (R_FAILED(status->session_rc)) {
        pt->session_rc = status->session_rc;
        return;
    }
    Service *srv = pctlGetServiceSession_Service();
    status_read_core(srv, status);
    if (pt->fw_supported) {
        pt->session_rc = status->session_rc;
        pt->session_valid = true;
        // 1006 was just read for the status: the same answer.
        pt->temporary_unlocked_valid = status->temp_unlocked_ok;
        pt->temporary_unlocked = status->temp_unlocked;
        pt->temporary_unlocked_rc = status->temp_unlocked_ok ? 0 : NXM_RC_STATE_UNKNOWN;
        pt_read_core(srv, pt);
        // Today warns while the "time's up" alarm is off.
        pt->alarm_disabled_valid = R_SUCCEEDED(rd_bool(srv, 1458, &pt->alarm_disabled));
    }
    pctl_ops_exit();
}

// One write of the play-timer block (195101), as the three writers below make
// it: the change check, the firmware, the arguments, then in one session the
// gate, the block as it is (145601) to start from, `edit`, the write and a
// read-back. A write the console did not take is undone with the block read.
typedef struct {
    const void *arg;
    bool (*args_ok)(const void *arg);   // NULL: nothing to check
    // Turns `c`, the block as read, into the one to write; a failure refuses
    // the write with that Result.
    Result (*edit)(u16 c[PT_U16_COUNT], const void *arg);
    // Whether the console took `c`, read after writing it: false puts back
    // the block as it was read (nothing when it could not be).
    bool (*took)(Service *srv, const u16 c[PT_U16_COUNT], const void *arg);
    // When 145601 fails: 0 starts from a zeroed block, anything else refuses
    // the write. NULL: refused (NXM_RC_STATE_UNKNOWN). `active` as
    // pt_write_gate says.
    Result (*unreadable)(bool active, const void *arg);
    bool write_unchanged;   // write even when `edit` changed nothing
} PtWrite;

static Result pt_write_in(Service *srv, const PtWrite *w)
{
    bool active = false;
    Result rc = pt_block_write_gate(srv, &active);
    if (R_FAILED(rc)) return rc;

    u16 before[PT_U16_COUNT], c[PT_U16_COUNT];
    memset(before, 0, sizeof(before));
    const bool have_current = R_SUCCEEDED(serviceDispatchOut(srv, 145601, before));
    // Read, but not a layout this app knows: writing from it could break what
    // it holds, and a zeroed block would wipe it.
    if (have_current && !pt_plausible(before)) return NXM_RC_PT_NOT_UNDERSTOOD;
    if (!have_current) {
        memset(before, 0, sizeof(before));
        rc = w->unreadable ? w->unreadable(active, w->arg) : NXM_RC_STATE_UNKNOWN;
        if (R_FAILED(rc)) return rc;
    }
    memcpy(c, before, sizeof(c));
    rc = w->edit(c, w->arg);
    if (R_FAILED(rc)) return rc;
    if (!w->write_unchanged && memcmp(c, before, sizeof(c)) == 0) return 0;
    rc = serviceDispatchIn(srv, 195101, c);
    if (R_FAILED(rc)) return rc;

    // 195101 is a debug command: "written" is never said of a block the
    // console did not take.
    if (w->took(srv, c, w->arg)) return 0;
    if (have_current) rc = serviceDispatchIn(srv, 195101, before);
    return R_FAILED(rc) ? rc : NXM_RC_NOT_APPLIED;
}

static Result pt_write(const PtWrite *w)
{
    pctl_ops_exit();   // nothing held while the PIN check may show its applet
    Result gate = core_change_allowed();
    if (R_FAILED(gate)) return gate;
    if (!pt_fw_supported()) return NXM_RC_FW_UNSUPPORTED;
    if (w->args_ok && !w->args_ok(w->arg)) return NXM_RC_INVALID_ARGUMENT;

    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) return rc;
    rc = pt_write_in(pctlGetServiceSession_Service(), w);
    pctl_ops_exit();
    return rc;
}

static bool days_args_ok(const void *arg) { return pt_days_ok((const u16 *)arg); }

static Result days_edit(u16 c[PT_U16_COUNT], const void *arg)
{
    pt_encode(c, (const u16 *)arg);
    return 0;
}

// The limits read back are the ones written. An unreadable block proves
// nothing either way: the write stands.
static bool days_took(Service *srv, const u16 c[PT_U16_COUNT], const void *arg)
{
    (void)arg;
    u16 back[PT_U16_COUNT], want[7], got[7];
    if (R_FAILED(serviceDispatchOut(srv, 145601, back))) return true;
    pt_decode(c, want);
    pt_decode(back, got);
    return memcmp(want, got, sizeof(want)) == 0;
}

// Start from the settings as they are, so the fields this app does not
// understand survive. Should the read fail while the timer is off, fall back
// to the layout observed on hardware (what earlier versions always wrote):
// there is nothing set to lose. While it is on, the block holds what the
// companion app set, which a zeroed block would wipe: refuse, unless no day
// keeps a limit (pt_encode then writes all zeros anyway).
static Result days_unreadable(bool active, const void *arg)
{
    const u16 *days_min = (const u16 *)arg;
    bool any = false;
    for (int n = 0; n < 7; n++) if (days_min[n] != PT_DAY_NOLIMIT) any = true;
    return active && any ? NXM_RC_STATE_UNKNOWN : 0;
}

Result pctl_play_timer_set_days(const u16 days_min[7])
{
    const PtWrite w = { .arg = days_min, .args_ok = days_args_ok, .edit = days_edit, .took = days_took,
                        .unreadable = days_unreadable, .write_unchanged = true };
    return pt_write(&w);
}

typedef struct {
    const PtBedtime *bed;
    int today;
} BedtimeArg;

static bool bedtime_args_ok(const void *arg)
{
    const BedtimeArg *a = (const BedtimeArg *)arg;
    if (a->today < 0 || a->today > 6) return false;
    for (int n = 0; n < 7; n++)
        if (!pt_bedtime_ok(&a->bed[n])) return false;
    return true;
}

// Only ever from the block as it is (no `unreadable`): a zeroed one would
// wipe the limits.
static Result bedtime_edit(u16 c[PT_U16_COUNT], const void *arg)
{
    pt_bedtime_encode(c, ((const BedtimeArg *)arg)->bed);
    return 0;
}

// What 1954/1956/1957 report is `b` (the time only matters while it is on).
static bool bedtime_reported(const PtBedtime *b, bool on, u8 h, u8 m)
{
    return on == b->on && (!on || (h == b->hour && m == b->minute));
}

// Today's bedtime or yesterday's (still running past midnight) is reported;
// one that cannot be read is not taken.
static bool bedtime_took(Service *srv, const u16 c[PT_U16_COUNT], const void *arg)
{
    (void)c;
    const BedtimeArg *a = (const BedtimeArg *)arg;
    bool on = false;
    u8 h = 0, m = 0;
    const bool read = R_SUCCEEDED(rd_bool(srv, 1954, &on)) && R_SUCCEEDED(rd_u8(srv, 1956, &h)) &&
                      R_SUCCEEDED(rd_u8(srv, 1957, &m));
    return read && (bedtime_reported(&a->bed[a->today], on, h, m) ||
                    bedtime_reported(&a->bed[(a->today + 6) % 7], on, h, m));
}

Result pctl_play_timer_set_bedtime(const PtBedtime bed[7], int today)
{
    const BedtimeArg a = { bed, today };
    const PtWrite w = { .arg = &a, .args_ok = bedtime_args_ok, .edit = bedtime_edit, .took = bedtime_took };
    return pt_write(&w);
}

// Only ever from the block as it is, as the bedtime.
static Result alarm_only_edit(u16 c[PT_U16_COUNT], const void *arg)
{
    return pt_set_suspends(c, !*(const bool *)arg) ? 0 : NXM_RC_INVALID_ARGUMENT;
}

// An unreadable block proves nothing either way: the write stands.
static bool alarm_only_took(Service *srv, const u16 c[PT_U16_COUNT], const void *arg)
{
    (void)c;
    u16 back[PT_U16_COUNT];
    return !(R_SUCCEEDED(serviceDispatchOut(srv, 145601, back)) && pt_suspends(back) == *(const bool *)arg);
}

Result pctl_play_timer_set_alarm_only(bool alarm_only)
{
    const PtWrite w = { .arg = &alarm_only, .edit = alarm_only_edit, .took = alarm_only_took };
    return pt_write(&w);
}

Result pctl_play_timer_set_alarm_disabled(bool disabled)
{
    const u8 v = disabled ? 1 : 0;
    return pt_gated_command(1953, &v);
}

Result pctl_play_timer_start(void) { return pt_gated_command(1451, NULL); }
Result pctl_play_timer_stop(void)  { return pt_gated_command(1452, NULL); }

// Every read of a PtSample failed with `rc`.
static void pt_sample_failed(PtSample *out, Result rc)
{
    out->session_rc = out->unlocked_rc = out->enabled_rc = out->restricted_rc = out->alarm_off_rc =
        out->remaining_rc = out->block_rc = out->bedtime_rc = out->display_rc = out->spent_rc =
            out->extra_rc = rc;
}

void pctl_play_timer_sample(PtSample *out)
{
    memset(out, 0, sizeof(*out));
    // Below 21.0.0 no play-timer command at all, as the diagnostic report.
    if (!pt_fw_supported()) {
        pt_sample_failed(out, NXM_RC_FW_UNSUPPORTED);
        return;
    }
    Result rc = pctl_ops_reinit();
    if (R_FAILED(rc)) {
        pt_sample_failed(out, rc);
        return;
    }
    Service *srv = pctlGetServiceSession_Service();
    out->unlocked_rc   = rd_bool(srv, 1006, &out->unlocked);
    out->enabled_rc    = rd_bool(srv, 1453, &out->enabled);
    out->restricted_rc = rd_bool(srv, 1455, &out->restricted);
    out->alarm_off_rc  = rd_bool(srv, 1458, &out->alarm_off);
    out->remaining_rc  = rd_u64(srv, 1454, (u64 *)&out->remaining_ns);   // signed
    out->block_rc      = serviceDispatchOut(srv, 145601, out->block);
    out->bedtime_rc    = rd_bool(srv, 1954, &out->bedtime_on);
    if (R_SUCCEEDED(out->bedtime_rc)) out->bedtime_rc = rd_u8(srv, 1956, &out->bedtime_hour);
    if (R_SUCCEEDED(out->bedtime_rc)) out->bedtime_rc = rd_u8(srv, 1957, &out->bedtime_minute);

    // The test / debug getters in their own session, as the report reads them.
    out->session_rc = pctl_ops_reinit();
    if (R_FAILED(out->session_rc)) {
        out->display_rc = out->spent_rc = out->extra_rc = out->session_rc;
        return;
    }
    srv = pctlGetServiceSession_Service();
    out->display_rc = serviceDispatchOut(srv, 1459, out->display);
    out->spent_rc   = rd_u64(srv, 1952, &out->spent_ns);
    out->extra_rc   = rd_u64(srv, 1960, &out->extra_ns);
    pctl_ops_exit();
}
