// bl_ntp: the request, and each check made on a reply.
// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include <stdio.h>
#include <string.h>

#include "bl_ntp.h"
#include "check.h"

static const uint8_t COOKIE[BL_NTP_COOKIE_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8 };

// A reply of version 4 and stratum 2 for NTP time `secs`, answering COOKIE.
static void reply(uint8_t p[BL_NTP_PACKET_SIZE], uint32_t secs)
{
    memset(p, 0, BL_NTP_PACKET_SIZE);
    p[0] = (4u << 3) | 4u;
    p[1] = 2;
    memcpy(p + 24, COOKIE, BL_NTP_COOKIE_SIZE);
    p[40] = (uint8_t)(secs >> 24);
    p[41] = (uint8_t)(secs >> 16);
    p[42] = (uint8_t)(secs >> 8);
    p[43] = (uint8_t)secs;
}

static void test_request(void)
{
    uint8_t r[BL_NTP_PACKET_SIZE];
    bl_ntp_make_request(r, COOKIE);
    CHECK(r[0] == 0x23 && !memcmp(r + 40, COOKIE, 8) && r[1] == 0 && r[39] == 0);
}

static void test_parse(void)
{
    uint8_t p[BL_NTP_PACKET_SIZE];
    uint64_t t = 1;
    const uint32_t secs = (uint32_t)(1790000000ull + 2208988800ull);   // a day of 2026, in NTP time
    reply(p, secs);
    CHECK(bl_ntp_parse(p, sizeof(p), COOKIE, &t) == BlNtp_Ok && t == 1790000000ull);

    CHECK(bl_ntp_parse(p, 47, COOKIE, &t) == BlNtp_TooShort && t == 0);
    CHECK(bl_ntp_parse(NULL, 48, COOKIE, &t) == BlNtp_BadArgument);
    uint8_t q[BL_NTP_PACKET_SIZE];
    reply(q, secs);
    q[0] |= 0xC0;
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_Unsynchronized);
    reply(q, secs);
    q[0] = (2u << 3) | 4u;
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_BadVersion);
    reply(q, secs);
    q[0] = (4u << 3) | 3u;
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_BadMode);
    reply(q, secs);
    q[1] = 0;
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_BadStratum);
    reply(q, secs);
    q[24] ^= 0xFF;
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_WrongOrigin);
    reply(q, 0);
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_ZeroTimestamp);
    reply(q, (uint32_t)(1500000000ull + 2208988800ull));   // 2017: before the window, and no era makes it fit
    CHECK(bl_ntp_parse(q, sizeof(q), COOKIE, &t) == BlNtp_OutOfRange);
    CHECK(bl_ntp_error(BlNtp_WrongOrigin)[0] != '\0' && bl_ntp_error(BlNtp_Ok)[0] == '\0');
}

int main(void)
{
    test_request();
    test_parse();
    return CHECK_DONE("ntp: assertions passed");
}
