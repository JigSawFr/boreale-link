// sync_apply — an order received on `<prefix>/<id>/<entity>/set` turned into
// a SyncIntent, checked and bounded. Payloads are plain scalars ("90", "ON",
// "21:30", a profile name): there is no JSON on the order side, so nothing to
// parse that could surprise. There is deliberately no intent that deletes
// parental controls, unlinks the companion app, or reads or changes the PIN:
// the host tests check every entity the parser accepts.
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_ENTITY_MAX   40
#define SYNC_PAYLOAD_MAX  96
#define SYNC_PROFILE_MAX  33

// A day's limit on the wire: 0..1440 minutes, 1440 meaning "no limit".
#define SYNC_NO_LIMIT_MIN 1440u

typedef enum {
    SyncIntent_None = 0,
    // Play timer (need remote_timer_writes).
    SyncIntent_LimitDay,        // day, minutes (and days[day], mask)
    SyncIntent_LimitUniform,    // minutes (and days, mask 0x7F)
    SyncIntent_LimitsWeek,      // days, mask (several limit orders merged give a partial week)
    SyncIntent_LimitToday,      // minutes
    SyncIntent_RemoveLimit,
    SyncIntent_ConsoleLock,     // on
    SyncIntent_BonusTime,       // minutes
    SyncIntent_StopToday,
    SyncIntent_Unlock,          // on = unlocked
    SyncIntent_Alarm,           // on = the "time's up" alarm sounds
    SyncIntent_BedtimeEnabled,  // on
    SyncIntent_Bedtime,         // hour, minute: the alarm
    SyncIntent_BedtimeEnd,      // hour, minute: allowed again
    SyncIntent_Profile,         // profile
    // Restrictions and locking (no unlock involved).
    SyncIntent_LockNow,
    SyncIntent_Level,           // level (PctlSafetyLevel)
    SyncIntent_Vr,              // on = restricted
    SyncIntent_Sns,             // on = posting restricted (custom level only)
    SyncIntent_Comm,            // on = communication restricted (custom level only)
    // Handled by the engine or the host, not the console.
    SyncIntent_SyncNow,
    SyncIntent_ExportReport,
    SyncIntent_Discovery,       // on
} SyncIntentKind;

typedef struct {
    SyncIntentKind kind;
    int            day;          // 0 = Sunday
    uint16_t       minutes;      // limits: PT_DAY_NOLIMIT (0xFFFF) for "no limit"
    uint16_t       days[7];      // Sunday first, PT_DAY_NOLIMIT for "no limit"
    uint8_t        mask;         // the days of `days` that are set (bit 0 = Sunday)
    bool           on;
    uint8_t        hour, minute;
    uint32_t       level;
    char           profile[SYNC_PROFILE_MAX];
} SyncIntent;

typedef enum {
    SyncReason_None = 0,             // applied
    SyncReason_UnknownEntity,
    SyncReason_Invalid,              // the payload is not what the entity takes
    SyncReason_OutOfRange,
    SyncReason_PolicyOff,            // the link is set to publish only
    SyncReason_TimerWritesDisabled,  // remote_timer_writes is off
    SyncReason_ReadOnly,             // PlayGuard's read-only mode (untested firmware)
    SyncReason_NotConfirmed,         // declined on the console, or the PIN was not entered
    SyncReason_Gated,                // the timer counts down and could not be unlocked
    SyncReason_UnlockFailed,
    SyncReason_NotCustom,            // a custom restriction outside the custom level
    SyncReason_NoSuchProfile,
    SyncReason_NoLimitToday,         // extra time needs a limit today
    SyncReason_ConsoleLocked,        // extra time / no more play while the console lock is on
    SyncReason_Unsupported,          // firmware, or a bedtime the block does not hold as reported
    SyncReason_BedtimeOff,           // the end of the bedtime needs the bedtime on
    SyncReason_PctlError,            // the console refused; see rc
    SyncReason_Waiting,              // kept for PlayGuard to confirm (policy "ask")
    SyncReason_Busy,                 // PlayGuard could not take it now; try again
} SyncReason;

// Bytes of `payload` (not NUL-terminated) for `entity`. On success *out is
// filled; otherwise the reason (UnknownEntity, Invalid, OutOfRange).
SyncReason sync_apply_parse(const char *entity, const char *payload, size_t len, SyncIntent *out);

// The intent changes the play timer or unlocks: it needs remote_timer_writes.
bool sync_intent_needs_timer_writes(SyncIntentKind kind);
// The intent is carried out on the console (sync_exec), not by the engine or
// the host (SyncNow, Discovery, ExportReport).
bool sync_intent_on_console(SyncIntentKind kind);
// Whether the intent changes nothing on the console (SyncNow, ExportReport).
bool sync_intent_harmless(SyncIntentKind kind);

// "out_of_range" …, as the protocol names them.
const char *sync_reason_name(SyncReason r);

#ifdef __cplusplus
}
#endif
