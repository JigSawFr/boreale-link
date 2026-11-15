// rescue — request and report files (see rescue.h).
// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "rescue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

RescueMode rescue_request_mode(const char *content, size_t len)
{
    static const char word[] = "delete";
    const size_t n = sizeof(word) - 1;
    if (!content) return RescueMode_Unlock;
    size_t i = 0;
    // Notepad's byte-order mark, at the very start only.
    if (len >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF)
        i = 3;
    while (i < len) {
        size_t end = i;
        while (end < len && content[end] != '\n') end++;
        size_t b = i, e = end;
        while (b < e && is_space(content[b])) b++;
        while (e > b && is_space(content[e - 1])) e--;
        if (e - b == n) {
            size_t k = 0;
            while (k < n && lower(content[b + k]) == word[k]) k++;
            if (k == n) return RescueMode_Delete;
        }
        i = end + 1;
    }
    return RescueMode_Unlock;
}

const char *rescue_mode_name(RescueMode mode)
{
    return mode == RescueMode_Delete ? "delete" : "unlock";
}

const char *rescue_request_name(RescueRequest request)
{
    switch (request) {
    case RescueRequest_Removed: return "removed";
    case RescueRequest_Renamed: return "renamed";
    default:                    return "kept";
    }
}

const char *rescue_result_name(RescueResult result)
{
    switch (result) {
    case RescueResult_Ok:      return "ok";
    case RescueResult_NoPin:   return "no_pin";
    case RescueResult_Refused: return "refused";
    default:                   return "failed";
    }
}

size_t rescue_report_format(const RescueReport *r, char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    buf[0] = '\0';
    if (!r) return 0;
    int n = snprintf(buf, size, "mode=%s\nresult=%s\nrc=0x%08X\nunlocks=%u\nrequest=%s\n",
                     rescue_mode_name(r->mode), rescue_result_name(r->result),
                     (unsigned)r->rc, (unsigned)r->unlocks, rescue_request_name(r->request));
    if (n < 0 || (size_t)n >= size) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

// `value` (not NUL-terminated, `n` bytes) as an unsigned number: decimal, or
// hexadecimal after "0x". False for anything else or above 32 bits.
static bool parse_u32(const char *value, size_t n, uint32_t *out)
{
    char tmp[16];
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, value, n);
    tmp[n] = '\0';
    int base = 10;
    const char *digits = tmp;
    if (n > 2 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
        base = 16;
        digits = tmp + 2;
    }
    for (const char *c = digits; *c; c++) {
        const bool dec = *c >= '0' && *c <= '9';
        const bool hex = (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
        if (!(dec || (base == 16 && hex))) return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(digits, &end, base);
    if (!end || *end || v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

static bool equals(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && memcmp(s, word, n) == 0;
}

bool rescue_report_parse(const char *text, size_t len, RescueReport *out)
{
    if (!text || !out) return false;
    RescueReport r = { RescueMode_Unlock, RescueResult_Failed, 0, 0, RescueRequest_Removed };
    bool has_mode = false, has_result = false;
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        size_t line_end = end;
        if (line_end > i && text[line_end - 1] == '\r') line_end--;
        const char *line = text + i;
        const size_t n = line_end - i;
        const char *eq = memchr(line, '=', n);
        if (eq) {
            const size_t kn = (size_t)(eq - line);
            const char *v = eq + 1;
            const size_t vn = n - kn - 1;
            if (equals(line, kn, "mode")) {
                if (equals(v, vn, "unlock"))      r.mode = RescueMode_Unlock;
                else if (equals(v, vn, "delete")) r.mode = RescueMode_Delete;
                else return false;
                has_mode = true;
            } else if (equals(line, kn, "result")) {
                if (equals(v, vn, "ok"))          r.result = RescueResult_Ok;
                else if (equals(v, vn, "no_pin")) r.result = RescueResult_NoPin;
                else if (equals(v, vn, "failed")) r.result = RescueResult_Failed;
                else if (equals(v, vn, "refused")) r.result = RescueResult_Refused;
                else return false;
                has_result = true;
            } else if (equals(line, kn, "rc")) {
                if (!parse_u32(v, vn, &r.rc)) return false;
            } else if (equals(line, kn, "unlocks")) {
                if (!parse_u32(v, vn, &r.unlocks)) return false;
            } else if (equals(line, kn, "request")) {
                if (equals(v, vn, "removed"))      r.request = RescueRequest_Removed;
                else if (equals(v, vn, "renamed")) r.request = RescueRequest_Renamed;
                else if (equals(v, vn, "kept"))    r.request = RescueRequest_Kept;
                else return false;
            }
        }
        i = end + 1;
    }
    if (!has_mode || !has_result) return false;
    *out = r;
    return true;
}

bool rescue_report_needs_proof(const RescueReport *r)
{
    return r && r->result == RescueResult_Ok && r->mode == RescueMode_Unlock;
}

// `n` bytes as lower-case hex into buf (2n + 1 bytes, NUL-terminated).
static void to_hex(const uint8_t *bytes, size_t n, char *buf)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        buf[2 * i]     = HEX[bytes[i] >> 4];
        buf[2 * i + 1] = HEX[bytes[i] & 0xF];
    }
    buf[2 * n] = '\0';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Exactly 2n hex digits in `text` (`len` bytes) into out[n]; out is only
// written when they all are.
static bool from_hex(const char *text, size_t len, uint8_t *out, size_t n)
{
    if (len != 2 * n) return false;
    for (size_t i = 0; i < len; i++)
        if (hex_digit(text[i]) < 0) return false;
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(hex_digit(text[2 * i]) << 4 | hex_digit(text[2 * i + 1]));
    return true;
}

size_t rescue_nonce_format(const uint8_t nonce[RESCUE_NONCE_SIZE], char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    buf[0] = '\0';
    if (!nonce || size < 2 * RESCUE_NONCE_SIZE + 2) return 0;
    to_hex(nonce, RESCUE_NONCE_SIZE, buf);
    buf[2 * RESCUE_NONCE_SIZE]     = '\n';
    buf[2 * RESCUE_NONCE_SIZE + 1] = '\0';
    return 2 * RESCUE_NONCE_SIZE + 1;
}

bool rescue_nonce_parse(const char *text, size_t len, uint8_t out[RESCUE_NONCE_SIZE])
{
    if (!text || !out) return false;
    size_t b = 0, e = len;
    while (b < e && is_space(text[b])) b++;
    while (e > b && is_space(text[e - 1])) e--;
    return from_hex(text + b, e - b, out, RESCUE_NONCE_SIZE);
}

size_t rescue_mac_message(const RescueReport *r, const uint8_t nonce[RESCUE_NONCE_SIZE], char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    buf[0] = '\0';
    if (!r || !nonce) return 0;
    char hex[2 * RESCUE_NONCE_SIZE + 1];
    to_hex(nonce, RESCUE_NONCE_SIZE, hex);
    const int head = snprintf(buf, size, "playguard-rescue/2\nnonce=%s\n", hex);
    if (head < 0 || (size_t)head >= size) {
        buf[0] = '\0';
        return 0;
    }
    const size_t body = rescue_report_format(r, buf + head, size - (size_t)head);
    if (body == 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)head + body;
}

// Not optimised away, though the buffer is not read again.
static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

bool rescue_proof_mac(RescueHmac hmac, const uint8_t secret[RESCUE_SECRET_SIZE], const char *pin, size_t pin_len,
                      const void *msg, size_t msg_len, uint8_t mac[RESCUE_MAC_SIZE])
{
    if (!mac) return false;
    memset(mac, 0, RESCUE_MAC_SIZE);
    if (!hmac || !secret || !pin || !msg || pin_len == 0 || pin_len > 8) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < RESCUE_SECRET_SIZE; i++) any |= secret[i];
    if (!any) return false;
    static const char label[] = "playguard-rescue-key/2\n";
    const size_t label_len = strlen(label);
    char text[sizeof(label) + 8];
    memcpy(text, label, label_len);
    memcpy(&text[label_len], pin, pin_len);
    uint8_t key[RESCUE_MAC_SIZE];
    hmac(key, secret, RESCUE_SECRET_SIZE, text, label_len + pin_len);
    hmac(mac, key, sizeof(key), msg, msg_len);
    wipe(text, sizeof(text));
    wipe(key, sizeof(key));
    return true;
}

size_t rescue_proof_format(const RescueProof *p, char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    buf[0] = '\0';
    if (!p || !p->present) return 0;
    char nonce[2 * RESCUE_NONCE_SIZE + 1], mac[2 * RESCUE_MAC_SIZE + 1];
    to_hex(p->nonce, RESCUE_NONCE_SIZE, nonce);
    to_hex(p->mac, RESCUE_MAC_SIZE, mac);
    const int n = snprintf(buf, size, "nonce=%s\nmac=%s\n", nonce, mac);
    if (n < 0 || (size_t)n >= size) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

void rescue_proof_parse(const char *text, size_t len, RescueProof *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!text) return;
    RescueProof p;
    memset(&p, 0, sizeof(p));
    bool has_nonce = false, has_mac = false;
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        size_t line_end = end;
        if (line_end > i && text[line_end - 1] == '\r') line_end--;
        const char *line = text + i;
        const size_t n = line_end - i;
        const char *eq = memchr(line, '=', n);
        if (eq) {
            const size_t kn = (size_t)(eq - line);
            const char *v = eq + 1;
            const size_t vn = n - kn - 1;
            // A second line of either kind: not one the sysmodule wrote.
            if (equals(line, kn, "nonce")) {
                if (has_nonce || !from_hex(v, vn, p.nonce, RESCUE_NONCE_SIZE)) return;
                has_nonce = true;
            } else if (equals(line, kn, "mac")) {
                if (has_mac || !from_hex(v, vn, p.mac, RESCUE_MAC_SIZE)) return;
                has_mac = true;
            }
        }
        i = end + 1;
    }
    if (!has_nonce || !has_mac) return;
    p.present = true;
    *out = p;
}

bool rescue_mac_equal(const uint8_t a[RESCUE_MAC_SIZE], const uint8_t b[RESCUE_MAC_SIZE])
{
    if (!a || !b) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < RESCUE_MAC_SIZE; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

size_t rescue_pin_length(const char *pin, size_t size, uint32_t reported)
{
    if (!pin) return 0;
    size_t n = 0;
    while (n < size && pin[n]) n++;
    if (reported > 0 && reported < n) n = reported;
    return n;
}

bool rescue_report_confirmed(const RescueReport *r, uint32_t pin_length, bool unlocked)
{
    if (!r) return false;
    switch (r->result) {
    case RescueResult_Ok:
        return r->mode == RescueMode_Delete ? pin_length == 0 : pin_length > 0 && unlocked;
    case RescueResult_NoPin:
        return pin_length == 0;
    default:
        return false;   // nothing was done: nothing to confirm
    }
}
