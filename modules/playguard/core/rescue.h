// rescue — the "last chance" shared by the rescue sysmodule (sysmodule/) and
// the app: the request file a parent puts on the SD card, and the report the
// sysmodule leaves for PlayGuard.
//
// A parent who forgot the PIN, while the play timer keeps every program from
// starting (PlayGuard included), puts a file named RESCUE (or RESCUE.txt) in
// sd:/config/playguard/ from a computer and turns the console on:
//   - empty, or any text: the sysmodule unlocks parental controls temporarily
//     with the stored PIN (as the PIN screen would), so PlayGuard can open;
//   - a line that reads "delete" (any case, spaces around it ignored): the
//     sysmodule deletes every parental control.
// The request is removed before anything is done (it is used once); should
// that fail it is renamed RESCUE.done, and should that fail too a delete is
// not done (it would run again every boot) while an unlock still is. The
// result is left in rescue_report.txt for PlayGuard, which shows it, records
// it in the change history and removes the report.
//
// Anyone who can edit the SD card can do this, as they can turn off
// PlayGuard's own PIN prompt (pin_lock.hpp): it keeps a child out, not
// someone with a computer. No PIN is ever written to either file, nor
// anything it could be guessed back from on a computer: the report is a
// plain file anyone can write, so PlayGuard trusts it only when the console
// confirms it (rescue_report_confirmed) and, for an unlock, when it carries
// the sysmodule's proof (RescueProof): an answer to a challenge the app left
// on the card (RESCUE_NONCE_NAME), keyed with the stored PIN and a secret of
// the console that is not on the card (rescue_proof_mac).
//
// Plain C, no libnx: host-tested (tests/rescue).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// File names inside the data directory (sd:/config/playguard/). The SD card's
// file system ignores case, so "rescue.txt" is found too.
#define RESCUE_REQUEST_NAME     "RESCUE"
#define RESCUE_REQUEST_NAME_TXT "RESCUE.txt"
#define RESCUE_REQUEST_NAME_DONE "RESCUE.done"   // a request that could not be removed
#define RESCUE_REPORT_NAME      "rescue_report.txt"
// The app's challenge: RESCUE_NONCE_SIZE random bytes as hex digits, a new
// one written at every start of PlayGuard (a report answers the one before,
// once). The sysmodule reads it and never writes it.
#define RESCUE_NONCE_NAME       "rescue_nonce"
#define RESCUE_NONCE_SIZE       16
#define RESCUE_MAC_SIZE         32   // HMAC-SHA256
// The console's secret in the MAC's key (rescue_proof_mac): set:sys
// GetMiiAuthorId, 16 random bytes the console made when it was set up.
#define RESCUE_SECRET_SIZE      16
// Where the sysmodule is installed, from the SD card root.
#define RESCUE_SYSMODULE_PATH   "atmosphere/contents/4200000000505247/exefs.nsp"
// Next to it: left by PlayGuard when it updates the module, removed by the
// module at boot. While it is there, the module that runs is still the old
// one: the console has not restarted since.
#define RESCUE_RESTART_PATH     "atmosphere/contents/4200000000505247/restart_pending"

typedef enum {
    RescueMode_Unlock = 0,   // unlock temporarily with the stored PIN
    RescueMode_Delete = 1,   // delete every parental control (1043)
} RescueMode;

typedef enum {
    RescueResult_Ok     = 0,
    RescueResult_NoPin  = 1,   // nothing to unlock: no PIN is set
    RescueResult_Failed = 2,   // `rc` says why
    RescueResult_Refused = 3,  // a delete not done: the request could not be removed
} RescueResult;

// What became of the request file (RESCUE or RESCUE.txt) before acting on it,
// from best to worst (the sysmodule keeps the worse of the two names).
typedef enum {
    RescueRequest_Removed = 0,   // deleted, as it should be
    RescueRequest_Renamed = 1,   // could not be deleted: renamed RESCUE.done
    RescueRequest_Kept    = 2,   // could be neither: still there, acted on again next boot
} RescueRequest;

typedef struct {
    RescueMode   mode;
    RescueResult result;
    uint32_t     rc;        // the last failure, 0 when none
    uint32_t     unlocks;   // unlocks done (again when the system locked in between)
    RescueRequest request;  // "request=" line; Removed when absent (older reports)
} RescueReport;

// What shows the sysmodule wrote a report: its "nonce=" and "mac=" lines,
// after the others. `nonce` is the challenge it read (RESCUE_NONCE_NAME);
// `mac` is rescue_proof_mac of rescue_mac_message, keyed with the stored PIN
// (1208, rescue_pin_length digits) and the console's secret, which the app
// reads too to check it. Nothing that cannot read the PIN can make one.
// Absent (present false) from the reports of a sysmodule from before the
// proof, and when there was no challenge to answer; one from before the
// console's secret (keyed with the PIN alone, "playguard-rescue/1") never
// checks.
typedef struct {
    bool    present;
    uint8_t nonce[RESCUE_NONCE_SIZE];
    uint8_t mac[RESCUE_MAC_SIZE];
} RescueProof;

// The mode a request asks for: Delete when, in its first `len` bytes, a line
// reads "delete" in any case once ASCII spaces (and a UTF-8 byte-order mark at
// the start) are trimmed; else Unlock (an empty file included). "delete
// everything" or "undelete" is an unlock: deleting takes an unambiguous word.
RescueMode rescue_request_mode(const char *content, size_t len);

// "mode=unlock\nresult=ok\nrc=0x00000000\nunlocks=1\nrequest=removed\n" into buf. Returns the
// length written, or 0 when it does not fit (buf then holds "").
size_t rescue_report_format(const RescueReport *r, char *buf, size_t size);

// Back from rescue_report_format. Lines in any order, unknown keys and
// "\r" ignored; mode and result are required. False, and *out unchanged,
// for anything else.
bool rescue_report_parse(const char *text, size_t len, RescueReport *out);

// Whether the console's state confirms what the report says the sysmodule
// did, read at start-up: an unlock that worked leaves parental controls
// temporarily unlocked (1006), a delete that worked or a "no PIN" leaves no
// PIN (1206 == 0). A failure or a refusal changed nothing, so nothing
// confirms it. Only a confirmed report opens the recovery actions without the
// PIN: a report written by hand on the SD card proves nothing. A confirmed
// unlock still needs its proof (rescue_report_needs_proof).
bool rescue_report_confirmed(const RescueReport *r, uint32_t pin_length, bool unlocked);

// Whether `r` must carry a proof to be believed: an unlock that worked. It
// leaves the PIN set and the recovery actions (a new PIN, delete everything)
// go around it, while a console left temporarily unlocked by the parent
// confirms it just as well. A delete or a "no PIN" leaves no PIN to go
// around; a failure or a refusal opens nothing.
bool rescue_report_needs_proof(const RescueReport *r);

// The challenge file's content: the nonce as lower-case hex digits and "\n".
// Returns the length written, or 0 when it does not fit (buf then holds "").
size_t rescue_nonce_format(const uint8_t nonce[RESCUE_NONCE_SIZE], char *buf, size_t size);
// Back from it: the hex digits (either case), ASCII spaces around them
// ignored. False, and *out unchanged, for anything else.
bool rescue_nonce_parse(const char *text, size_t len, uint8_t out[RESCUE_NONCE_SIZE]);

// What the MAC covers: "playguard-rescue/2\n", "nonce=<hex>\n" and the report
// as rescue_report_format writes it. Both sides build it from the values, so
// a report parsed back gives the same bytes. Length, or 0 when it does not fit.
size_t rescue_mac_message(const RescueReport *r, const uint8_t nonce[RESCUE_NONCE_SIZE], char *buf, size_t size);

// HMAC-SHA256 as libnx's hmacSha256CalculateMac: `dst` gets 32 bytes. Both
// sides pass libnx's; the simulator and the tests their own.
typedef void (*RescueHmac)(void *dst, const void *key, size_t key_size, const void *msg, size_t size);

// The proof's MAC of `msg` (rescue_mac_message): HMAC-SHA256 keyed with
// HMAC-SHA256(secret, "playguard-rescue-key/2\n" + the PIN's digits). Keyed
// with the PIN alone, a copy of the report would give the PIN back on a
// computer in seconds (10^8 PINs at most); the console's secret is on the
// console only (in its settings, not in the PRODINFO backups Atmosphère
// leaves on the SD card), so that takes 2^128 tries. False, and `mac`
// zeroed, without a PIN (`pin_len` 0 or over 8) or with a secret of zeros
// (none read). The intermediate key is wiped.
bool rescue_proof_mac(RescueHmac hmac, const uint8_t secret[RESCUE_SECRET_SIZE], const char *pin, size_t pin_len,
                      const void *msg, size_t msg_len, uint8_t mac[RESCUE_MAC_SIZE]);

// "nonce=<32 hex>\nmac=<64 hex>\n" into buf, for a present proof; 0 (buf "")
// when it is absent or does not fit.
size_t rescue_proof_format(const RescueProof *p, char *buf, size_t size);
// The proof of a report's text. present is false (and the rest zero) unless
// both lines are there with the right number of hex digits.
void rescue_proof_parse(const char *text, size_t len, RescueProof *out);

// Whether two MACs are the same, in a time that does not depend on where
// they differ.
bool rescue_mac_equal(const uint8_t a[RESCUE_MAC_SIZE], const uint8_t b[RESCUE_MAC_SIZE]);

// The PIN's length in the GetPinCode (1208) buffer `pin` (`size` bytes):
// up to its first NUL, or `reported` (1208's own count) when that is shorter.
// The MAC key on both sides, so they agree on it.
size_t rescue_pin_length(const char *pin, size_t size, uint32_t reported);

const char *rescue_mode_name(RescueMode mode);         // "unlock" / "delete"
const char *rescue_result_name(RescueResult result);   // "ok" / "no_pin" / "failed" / "refused"
const char *rescue_request_name(RescueRequest request); // "removed" / "renamed" / "kept"

#ifdef __cplusplus
}
#endif
