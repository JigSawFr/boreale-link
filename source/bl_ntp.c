// Copyright (C) 2026 JigSawFr, (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "bl_ntp.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "bl_net.h"


static uint32_t read_be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

void bl_ntp_make_request(uint8_t request[BL_NTP_PACKET_SIZE], const uint8_t cookie[BL_NTP_COOKIE_SIZE])
{
    memset(request, 0, BL_NTP_PACKET_SIZE);
    request[0] = (4u << 3) | 3u;   // LI=0, NTPv4, client mode.
    memcpy(request + 40, cookie, BL_NTP_COOKIE_SIZE);
}

BlNtpResult bl_ntp_parse(const uint8_t *packet, size_t length, const uint8_t cookie[BL_NTP_COOKIE_SIZE],
                         uint64_t *unix_seconds)
{
    if (unix_seconds) *unix_seconds = 0;
    if (!packet || !cookie || !unix_seconds) return BlNtp_BadArgument;
    if (length < BL_NTP_PACKET_SIZE) return BlNtp_TooShort;
    if ((packet[0] >> 6) == 3) return BlNtp_Unsynchronized;
    const unsigned version = (packet[0] >> 3) & 7u;
    if (version != 3 && version != 4) return BlNtp_BadVersion;
    if ((packet[0] & 7u) != 4) return BlNtp_BadMode;
    if (packet[1] < 1 || packet[1] > 15) return BlNtp_BadStratum;
    if (memcmp(packet + 24, cookie, BL_NTP_COOKIE_SIZE) != 0) return BlNtp_WrongOrigin;

    const uint32_t seconds = read_be32(packet + 40);
    const uint32_t fraction = read_be32(packet + 44);
    if (seconds == 0 && fraction == 0) return BlNtp_ZeroTimestamp;

    // The bounded date interval selects one NTP era without trusting the local
    // clock: it is narrower than one era (2^32 seconds), so cannot be ambiguous.
    const uint64_t epoch_offset = UINT64_C(2208988800);
    const uint64_t earliest = UINT64_C(1577836800);   // 2020-01-01 UTC, inclusive.
    const uint64_t latest = UINT64_C(4102444800);     // 2100-01-01 UTC, exclusive.
    for (unsigned era = 0; era < 2; ++era) {
        const uint64_t ntp_seconds = ((uint64_t)era << 32) + seconds;
        if (ntp_seconds < epoch_offset) continue;
        const uint64_t candidate = ntp_seconds - epoch_offset;
        if (candidate >= earliest && candidate < latest) {
            *unix_seconds = candidate;
            return BlNtp_Ok;
        }
    }
    return BlNtp_OutOfRange;
}

const char *bl_ntp_error(BlNtpResult result)
{
    switch (result) {
    case BlNtp_Ok: return "";
    case BlNtp_BadArgument: return "invalid NTP parser argument";
    case BlNtp_TooShort: return "NTP response is shorter than 48 bytes";
    case BlNtp_Unsynchronized: return "NTP server is not synchronized";
    case BlNtp_BadVersion: return "NTP response version must be 3 or 4";
    case BlNtp_BadMode: return "NTP response is not in server mode";
    case BlNtp_BadStratum: return "NTP server stratum must be 1 through 15";
    case BlNtp_WrongOrigin: return "NTP response does not match the request";
    case BlNtp_ZeroTimestamp: return "NTP server returned an unknown timestamp";
    case BlNtp_OutOfRange: return "NTP time is outside 2020 through 2099";
    }
    return "unknown NTP parser error";
}

// A cookie the reply must echo: not secret, only unlikely to be guessed by
// a stray datagram. The tick counter and the address of the stack vary.
static void make_cookie(uint8_t cookie[BL_NTP_COOKIE_SIZE])
{
    const uint64_t a = bl_now_ms() * UINT64_C(0x9E3779B97F4A7C15) + (uint64_t)(uintptr_t)cookie;
    for (unsigned i = 0; i < BL_NTP_COOKIE_SIZE; i++) cookie[i] = (uint8_t)(a >> (i * 8));
    cookie[0] |= 1;   // never all zero
}

bool bl_ntp_sample(const char *host, int timeout_ms, unsigned max_addresses, uint64_t *unix_seconds,
                   uint64_t *received_ms, char *err, size_t err_size)
{
    err[0] = '\0';
    if (!host || !*host) {
        snprintf(err, err_size, "no NTP server");
        return false;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo *list = NULL;
    const int g = getaddrinfo(host, "123", &hints, &list);
    if (g != 0 || !list) {
        snprintf(err, err_size, "cannot find %s: %s", host, g ? gai_strerror(g) : "no address");
        return false;
    }
    bool ok = false;
    unsigned tried = 0;
    snprintf(err, err_size, "no usable NTP address");
    for (const struct addrinfo *ai = list; ai && tried < max_addresses && !ok; ai = ai->ai_next, tried++) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            snprintf(err, err_size, "socket: %s", strerror(errno));
            continue;
        }
        // Connected UDP: only this server's datagrams are received.
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            snprintf(err, err_size, "connect: %s", strerror(errno));
            close(fd);
            continue;
        }
        uint8_t cookie[BL_NTP_COOKIE_SIZE], request[BL_NTP_PACKET_SIZE], response[128];
        make_cookie(cookie);
        bl_ntp_make_request(request, cookie);
        if (send(fd, request, sizeof(request), 0) != (ssize_t)sizeof(request)) {
            snprintf(err, err_size, "send: %s", strerror(errno));
            close(fd);
            continue;
        }
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        const int r = poll(&pf, 1, timeout_ms);
        if (r <= 0) {
            snprintf(err, err_size, r == 0 ? "no NTP answer before the timeout" : "poll: %s", strerror(errno));
            close(fd);
            continue;
        }
        const ssize_t got = recv(fd, response, sizeof(response), 0);
        const uint64_t at = bl_now_ms();
        close(fd);
        if (got < 0) {
            snprintf(err, err_size, "recv: %s", strerror(errno));
            continue;
        }
        const BlNtpResult res = bl_ntp_parse(response, (size_t)got, cookie, unix_seconds);
        if (res != BlNtp_Ok) {
            snprintf(err, err_size, "%s", bl_ntp_error(res));
            continue;
        }
        *received_ms = at;
        err[0] = '\0';
        ok = true;
    }
    freeaddrinfo(list);
    return ok;
}
