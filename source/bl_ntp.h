// bl_ntp — one network time sample over UDP (SNTP), read-only: it does not
// set any clock. The 48-byte request and the checks on the reply (version,
// mode, stratum, our origin cookie, a time in range) are plain C, tested on
// the host; bl_ntp_sample() needs the socket service up (a sysmodule calls
// socketInitialize). Adapted from anbingxi/NX-Pctl-Manager (diag/fw22-5-readonly).
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_NTP_PACKET_SIZE 48u
#define BL_NTP_COOKIE_SIZE 8u

typedef enum {
    BlNtp_Ok = 0,
    BlNtp_BadArgument,
    BlNtp_TooShort,
    BlNtp_Unsynchronized,
    BlNtp_BadVersion,
    BlNtp_BadMode,
    BlNtp_BadStratum,
    BlNtp_WrongOrigin,
    BlNtp_ZeroTimestamp,
    BlNtp_OutOfRange,
} BlNtpResult;

void        bl_ntp_make_request(uint8_t request[BL_NTP_PACKET_SIZE], const uint8_t cookie[BL_NTP_COOKIE_SIZE]);
BlNtpResult bl_ntp_parse(const uint8_t *packet, size_t length, const uint8_t cookie[BL_NTP_COOKIE_SIZE],
                         uint64_t *unix_seconds);
const char *bl_ntp_error(BlNtpResult result);

// Asks `host` (port 123), trying at most `max_addresses` of its addresses and
// waiting `timeout_ms` for each. On success: the server's time in Unix
// seconds, and the monotonic millisecond (bl_now_ms) at which the reply
// arrived, so the caller can age it up to the moment it read its own clocks.
// On failure `err` says why (English).
bool bl_ntp_sample(const char *host, int timeout_ms, unsigned max_addresses, uint64_t *unix_seconds,
                   uint64_t *received_ms, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
