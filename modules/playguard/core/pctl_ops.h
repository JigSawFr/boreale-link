// PlayGuard — wrappers around the system parental-control (`pctl`) service.
// All user-facing strings live in the UI layer; this layer is data only.
//
// Session ownership: `pctl:a` accepts a single session. Holding it while the
// system wants it (HOME-menu PIN prompt, pctlauth applet) destabilised
// Atmosphère on 22.5.0, so every public function below acquires the session,
// does its work and releases it before returning — including on error paths.
//
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See the
// LICENSE file or <https://www.gnu.org/licenses/gpl-3.0.html> for details.
#pragma once
#include "core/nx_types.h"
#include "pure.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PctlSafetyLevel_None       = 0,
    PctlSafetyLevel_Custom     = 1,
    PctlSafetyLevel_YoungChild = 2,
    PctlSafetyLevel_Child      = 3,
    PctlSafetyLevel_Teen       = 4,
} PctlSafetyLevel;

// Mirror of libnx's PctlRestrictionSettings (3 bytes on the wire). Kept under a
// different name so this header also builds where libnx is absent.
typedef struct {
    u8   rating_age;                       // 0 == no age restriction
    bool sns_post_restriction;             // true == posting screenshots to social media is restricted
    bool free_communication_restriction;   // true == communicating with other players is restricted
} PctlCustomSettings;

// ---- session ----
Result pctl_ops_init(void);    // idempotent; marks ownership only on success
void   pctl_ops_exit(void);    // releases only this layer's reference
Result pctl_ops_reinit(void);  // release, then acquire once (recovers a session the sysmodule closed)

// ---- overall status ----
// Every field has an *_ok flag: false means "that query failed / is not
// available on this firmware" — show "unavailable", never a default value.
typedef struct {
    Result session_rc;

    bool safety_level_ok;          u32  safety_level;            // 1032
    bool pin_length_ok;            u32  pin_length;              // 1206 (0 == no PIN)
    bool restriction_enabled_ok;   bool restriction_enabled;     // 1031
    bool temp_unlocked_ok;         bool temp_unlocked;           // 1006
    bool pairing_active_ok;        bool pairing_active;          // 1403
    bool settings_ok;              PctlCustomSettings settings;  // 1035
    bool rating_org_ok;            u32  rating_org;              // 1037
    bool stereo_vision_ok;         bool stereo_vision_restricted;// 1062 [4.0.0+]
    bool free_comm_count_ok;       u32  free_comm_count;         // 1039
    bool last_updated_ok;          u64  last_updated;            // 1406 (POSIX seconds)
} PctlStatus;

void pctl_status_fetch(PctlStatus *out);

// The two reads the rescue sysmodule repeats (core/rescue.h), in one short
// session: the PIN length (1206, 0 == no PIN) and whether parental controls
// are temporarily unlocked (1006). A pointer may be NULL to skip its read.
Result pctl_lock_state(u32 *pin_length, bool *unlocked);

const char *pctl_rating_org_name(u32 org);           // "PEGI", "ESRB", … or "?"
#define RATING_ORG_COUNT 13   // nn::ns::RatingOrganization, as pctl_rating_org_name names them

// ---- PIN / unlock ----
// Opens the system PIN applet (registers / changes it). NXM_RC_CANCELLED when
// it was left with B (PCTL_RC_AUTH_CANCELLED): nothing to say then.
Result pctl_set_pin(void);
// The system's PIN screen (pctlauth, as System Settings shows it), to confirm
// it is the parent: 0 when the right PIN was entered, NXM_RC_NO_PIN (nothing
// shown) when none is set, NXM_RC_CANCELLED when it was left with B, another
// error otherwise. Changes
// nothing, so read-only mode and the change check do not apply.
Result pctl_ask_pin(void);

// UnlockRestrictionTemporarily (1201) using the stored PIN read via GetPinCode
// (1208), then verifies IsRestrictionTemporaryUnlocked (1006). Returns
// NXM_RC_UNLOCK_NOT_EFFECTIVE when 1201 succeeded but 1006 still reads false.
// When 1006 itself fails after 1201, locks again (1007) and returns that error;
// NXM_RC_RELOCK_FAILED when 1007 fails too (the console may still be unlocked).
Result pctl_unlock_restriction_temporarily(void);
// GetPinCode (1208), for the "Show PIN" action: copies the stored PIN (4 to 8
// digits, NUL-terminated) into `out`. `out` is zeroed first and stays zeroed
// on any error. The caller wipes it after use; never log or report it.
Result pctl_get_pin(char *out, size_t out_size);
// Whether the stored PIN is one anyone would try first (pin_is_weak). The PIN
// is read (1208), judged and wiped here: it never leaves this function, so
// no reveal check is asked (nothing is shown). NXM_RC_NO_PIN with no PIN.
Result pctl_pin_weak(bool *weak);
// rescue_proof_mac of `msg` (`size` bytes), keyed with the stored PIN (1208)
// and the console's secret (set:sys GetMiiAuthorId), into `mac` (32 bytes,
// zeroed on any error): what checks the recovery report's proof
// (core/rescue.h). As above, the PIN is read, used and wiped here and nothing
// is shown, so no reveal check is asked. NXM_RC_NO_PIN with no PIN,
// NXM_RC_STATE_UNKNOWN when the secret reads as zeros or the PIN as no 1 to 8
// digits.
Result pctl_pin_hmac(const void *msg, size_t size, u8 *mac);
// RevertRestrictionTemporaryUnlocked (1007): re-applies restrictions now.
Result pctl_relock(void);

// ---- destructive ----
Result pctl_delete_parental_controls(void);  // 1043: wipes the PIN and every restriction — CANNOT BE UNDONE
Result pctl_delete_pairing(void);            // 1941: unlinks the companion phone app

// ---- restrictions ----
// GetSafetyLevelSettings (1034): what a level restricts (for the presets:
// Young Child / Child / Teen), to show before choosing it. Read only.
Result pctl_get_level_settings(u32 level, PctlCustomSettings *out);
Result pctl_set_safety_level(u32 level);                       // 1033
Result pctl_set_rating_org(u32 org);                           // 1038 SetDefaultRatingOrganization, org < 13
Result pctl_set_custom_settings(const PctlCustomSettings *s);  // 1036 (only when level == Custom)
Result pctl_set_stereo_vision_restricted(bool restricted);     // 1063 [4.0.0+]

// ---- play timer ----
// Per-day value sentinel: no configured limit at all for that day.
// (0 is a configured 0-minute limit; the two are different.)
#define PT_DAY_NOLIMIT 0xFFFFu

typedef struct {
    bool   session_valid;  Result session_rc;
    bool   fw_supported;   // false below 21.0.0: nothing else is filled in

    // 145601 GetPlayTimerSettings; not valid either (NXM_RC_PT_NOT_UNDERSTOOD)
    // when the block read fails pt_plausible.
    bool   valid;                 Result config_rc;
    u16    day_min[7];            // Sun..Sat minutes or PT_DAY_NOLIMIT; meaningful only when valid
    u16    block[PT_U16_COUNT];   // the 0x44 block as read (backups keep it; pure.h)
    PtBedtime bed[7];             // Sun..Sat bedtimes decoded from it; meaningful only when valid
    bool   alarm_only;            // restrictionMode ALARM: at the limit a warning only, the software
                                  // not suspended (pure.h); meaningful only when valid
    bool   enabled_valid;         Result enabled_rc;     bool enabled;     // 1453
    bool   restricted_valid;      Result restricted_rc;  bool restricted;  // 1455
    bool   temporary_unlocked_valid; Result temporary_unlocked_rc; bool temporary_unlocked; // 1006
    // 1454, a TimeSpan: below 0 once more was played than today's limit
    // (the console only warning, alarm_only, or parental controls unlocked):
    // minus the time over it. 1454 + 1952 still make the limit (observed,
    // 22.0.0: -224 s and 224 s on a 0-minute day).
    bool   remaining_valid;       Result remaining_rc;   s64  remaining_ns;// 1454
    bool   alarm_disabled_valid;  bool alarm_disabled;                     // 1458
    bool   bedtime_valid;         bool bedtime_enabled;  u8 bedtime_hour, bedtime_minute; // 1954/1956/1957
    bool   bedtime_reset_valid;   u8   bedtime_reset_hour, bedtime_reset_minute;          // 1958/1959 [20.0.0+]
} PtState;

void pctl_play_timer_query(PtState *out);

// Today's periodic read, in one session: of PctlStatus only safety
// level, PIN length, restriction enabled, temporarily unlocked and pairing;
// of PtState everything but the bedtime reset time (1958/1959). 1006 is read
// once for both. What is not read stays *_ok false.
void pctl_overview_fetch(PctlStatus *status, PtState *pt);

// The play-timer write gate: every write below (set_days and the functions
// built on it, set_alarm_disabled, start, stop) re-checks 1453/1455/1006 in its
// own session right before writing and refuses (NXM_RC_WRITE_GATED) when the
// timer is active (enabled or restricting) and not temporarily unlocked.
// The three that write the block (195101: set_days, set_bedtime,
// set_alarm_only) also refuse (NXM_RC_NO_PIN) while 1206 reads no PIN: pctl
// skips that write then, and still answers OK.
//
// days_min[0]=Sunday .. [6]=Saturday. If every day is PT_DAY_NOLIMIT the timer is
// turned off. Reads the current settings (145601) first and changes only the
// per-day limits, so what the companion app set in fields this app does not
// decode is kept. Should that read fail, writes the layout observed on hardware
// while the timer is off, and refuses (NXM_RC_STATE_UNKNOWN) while it is active
// unless no day keeps a limit (all zeros is written then, whatever was read).
// A block read that fails pt_plausible is refused (NXM_RC_PT_NOT_UNDERSTOOD),
// as by the bedtime write below.
Result pctl_play_timer_set_days(const u16 days_min[7]);
// No limit on any day: the timer off. Inline, so the simulator has it too.
static inline Result pctl_play_timer_clear(void)
{
    u16 d[7];
    for (int i = 0; i < 7; i++) d[i] = PT_DAY_NOLIMIT;
    return pctl_play_timer_set_days(d);
}

// Bedtime, Sun..Sat (pure.h): written into the block read with 145601 (only
// the bedtime bytes change), behind the same gate. The block layout is
// inferred, so the write is checked: right after it, 1954/1956/1957 must
// report the bedtime of `today` (0 = Sunday) or of the day before (after
// midnight the console may still be on the evening's). Otherwise the block
// read before is written back and NXM_RC_NOT_APPLIED returned (or the error
// of that write back). Refuses (NXM_RC_STATE_UNKNOWN) when the block cannot
// be read, NXM_RC_INVALID_ARGUMENT outside pt_bedtime_ok. Nothing is written
// when the bedtimes already are these.
Result pctl_play_timer_set_bedtime(const PtBedtime bed[7], int today);

// What the console does when the time is up (restrictionMode, pure.h):
// suspend the software, or show a warning only. Written into the block read
// with 145601 (only that byte changes), behind the same gate, then read back:
// a block the console does not keep is written back, NXM_RC_NOT_APPLIED.
// Refuses (NXM_RC_STATE_UNKNOWN) when the block cannot be read,
// NXM_RC_INVALID_ARGUMENT while no limit and no bedtime is set (pt_set_suspends).
// Nothing is written when it already is that.
Result pctl_play_timer_set_alarm_only(bool alarm_only);

// Advanced (debug-class commands; exposed behind the "advanced" switch). Gated
// as above.
Result pctl_play_timer_set_alarm_disabled(bool disabled);  // 1953
Result pctl_play_timer_start(void);                        // 1451
Result pctl_play_timer_stop(void);                         // 1452

// One reading of everything the play timer reports, for the developer
// recorder (util/pt_log): 1006, 1453, 1455, 1458, 1454, 145601 and
// 1954/1956/1957 in one session, then 1459, 1952 and 1960 in a second one,
// as the diagnostic report reads them. Read only. Each *_rc is that command's
// result; a value is meaningful only when its rc succeeded. A session that
// cannot be opened gives its error to every read it held, and below 21.0.0
// nothing is read: every rc is NXM_RC_FW_UNSUPPORTED.
typedef struct {
    Result session_rc;
    Result unlocked_rc, enabled_rc, restricted_rc, alarm_off_rc, remaining_rc, block_rc,
           bedtime_rc, display_rc, spent_rc, extra_rc;
    bool   unlocked, enabled, restricted, alarm_off, bedtime_on;   // 1006 1453 1455 1458 1954
    u8     bedtime_hour, bedtime_minute;                           // 1956 / 1957
    s64    remaining_ns;                                           // 1454 (signed, as in PtState)
    u64    spent_ns, extra_ns;                                     // 1952 1960
    u8     display[0x20];                                          // 1459 [20.0.0+]
    u16    block[PT_U16_COUNT];                                    // 145601 [21.0.0+]
} PtSample;

void pctl_play_timer_sample(PtSample *out);

// Writes a multi-line read-only diagnostic report (raw rc + bytes for every
// command above plus 1459/1460/1952/1960). PIN contents are never included.
void pctl_dump(char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif
