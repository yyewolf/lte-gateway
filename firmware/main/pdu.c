#include "pdu.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// ---------- GSM 03.38 default alphabet ----------
static const uint16_t GSM7[128] = {
    '@', 0xA3, '$', 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, '\n', 0xD8, 0xF8, '\r', 0xC5, 0xE5,
    0x394, '_', 0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
    ' ', '!', '"', '#', 0xA4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    0xA1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
    0xBF, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};
// Extension table (after ESC 0x1B)
static const struct { uint8_t code; uint16_t uc; } GSM7_EXT[] = {
    { 0x0A, 0x0C }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2F, '\\' },
    { 0x3C, '[' }, { 0x3D, '~' }, { 0x3E, ']' }, { 0x40, '|' }, { 0x65, 0x20AC },
};
#define N_EXT (sizeof(GSM7_EXT) / sizeof(GSM7_EXT[0]))

// Septets needed for code point cp in GSM-7 (1 or 2), 0 if not representable.
static int gsm7_lookup(uint32_t cp, uint8_t *code, bool *ext)
{
    if (cp != 0x1B) {
        for (int i = 0; i < 128; i++)
            if (GSM7[i] == cp) { *code = i; *ext = false; return 1; }
    }
    for (size_t i = 0; i < N_EXT; i++)
        if (GSM7_EXT[i].uc == cp) { *code = GSM7_EXT[i].code; *ext = true; return 2; }
    return 0;
}

// ---------- UTF-8 ----------
// Decodes one code point, returns bytes consumed (0 at end / invalid).
static int utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *)s;
    if (!u[0]) return 0;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    return 0;
}

static size_t utf8_put(char *out, size_t pos, size_t cap, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (pos + n >= cap) return pos;
    memcpy(out + pos, b, n);
    out[pos + n] = 0;
    return pos + n;
}

// ---------- hex ----------
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = toupper((unsigned char)c);
    return (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int cap)
{
    int n = 0;
    while (hex[0] && hex[1] && n < cap) {
        int h = hexv(hex[0]), l = hexv(hex[1]);
        if (h < 0 || l < 0) break;
        out[n++] = h << 4 | l;
        hex += 2;
    }
    return n;
}

static void put_hex(char *out, int *pos, uint8_t b)
{
    static const char H[] = "0123456789ABCDEF";
    out[(*pos)++] = H[b >> 4];
    out[(*pos)++] = H[b & 15];
    out[*pos] = 0;
}

// ---------- decode ----------
static int bcd_digit(uint8_t b, int hi) { return hi ? b >> 4 : b & 15; }

static void decode_address(const uint8_t *p, int ndigits, uint8_t toa, char *out, size_t cap)
{
    size_t o = 0;
    if ((toa & 0x70) == 0x50) {                 // alphanumeric: GSM-7 packed
        int septets = ndigits * 4 / 7;
        for (int i = 0; i < septets && o + 4 < cap; i++) {
            int bit = i * 7, byte = bit / 8, sh = bit % 8;
            int v = (p[byte] >> sh) | (sh > 1 ? (p[byte + 1] << (8 - sh)) : 0);
            o = utf8_put(out, o, cap, GSM7[v & 0x7F]);
        }
        out[o] = 0;
        return;
    }
    if ((toa & 0x70) == 0x10 && o + 1 < cap) out[o++] = '+';
    for (int i = 0; i < ndigits && o + 1 < cap; i++) {
        int d = bcd_digit(p[i / 2], i & 1);
        out[o++] = d < 10 ? '0' + d : d == 10 ? '*' : d == 11 ? '#' : 'a' + d - 12;
    }
    out[o] = 0;
}

static int bcd2(uint8_t b) { return (b & 15) * 10 + (b >> 4); }

static void decode_scts(const uint8_t *p, char *out, size_t cap)
{
    int tz = (p[6] & 0x07) * 10 + (p[6] >> 4);       // quarters of an hour, bit 3 = sign
    int neg = p[6] & 0x08;
    snprintf(out, cap, "20%02d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", bcd2(p[0]), bcd2(p[1]), bcd2(p[2]),
             bcd2(p[3]), bcd2(p[4]), bcd2(p[5]), neg ? '-' : '+', tz / 4, (tz % 4) * 15);
}

int pdu_decode(const char *hex, pdu_deliver_t *out)
{
    uint8_t b[200];
    int n = hex_to_bytes(hex, b, sizeof(b));
    memset(out, 0, sizeof(*out));
    if (n < 2) return -1;
    int p = 1 + b[0];                            // skip SMSC
    if (p >= n) return -1;
    uint8_t fo = b[p++];
    int mti = fo & 3;
    if (mti == 2) {                              // SMS-STATUS-REPORT: MR, RA, SCTS, DT, ST
        out->status_report = true;
        if (p + 2 > n) return -1;
        int mr = b[p++];
        int ndig = b[p++];
        if (p + 1 + (ndig + 1) / 2 + 15 > n) return -1;
        uint8_t toa = b[p++];
        decode_address(b + p, ndig, toa, out->sender, sizeof(out->sender));
        p += (ndig + 1) / 2;
        decode_scts(b + p, out->timestamp, sizeof(out->timestamp));
        p += 14;
        snprintf(out->text, sizeof(out->text), "mr=%d status=%d", mr, b[p]);
        return 0;
    }
    if (mti != 0) return -1;                     // only SMS-DELIVER otherwise
    bool udhi = fo & 0x40;
    if (p + 2 > n) return -1;
    int ndig = b[p++];
    uint8_t toa = b[p++];
    int alen = (ndig + 1) / 2;
    if (p + alen + 2 + 7 + 1 > n) return -1;
    decode_address(b + p, ndig, toa, out->sender, sizeof(out->sender));
    p += alen;
    p++;                                         // PID
    uint8_t dcs = b[p++];
    decode_scts(b + p, out->timestamp, sizeof(out->timestamp));
    p += 7;
    int udl = b[p++];
    const uint8_t *ud = b + p;
    int ud_bytes = n - p;

    // Alphabet from the DCS (general data coding + message waiting groups)
    int alphabet = 0;                            // 0 GSM-7, 1 8-bit, 2 UCS-2
    if ((dcs & 0xC0) == 0x00 || (dcs & 0xC0) == 0x40) alphabet = (dcs >> 2) & 3;
    else if ((dcs & 0xF0) == 0xE0) alphabet = 2;
    else if ((dcs & 0xF0) == 0xF0) alphabet = (dcs & 0x04) ? 1 : 0;
    if (alphabet == 3) alphabet = 1;

    int udh_len = 0;                             // bytes, including the UDHL byte
    if (udhi && ud_bytes > 0) {
        udh_len = ud[0] + 1;
        if (udh_len > ud_bytes) return -1;
        for (int i = 1; i + 1 < udh_len;) {
            uint8_t iei = ud[i], iel = ud[i + 1];
            if (i + 2 + iel > udh_len) break;
            if (iei == 0x00 && iel == 3) {
                out->concat = true;
                out->concat_ref = ud[i + 2];
                out->concat_total = ud[i + 3];
                out->concat_seq = ud[i + 4];
            } else if (iei == 0x08 && iel == 4) {
                out->concat = true;
                out->concat_ref = ud[i + 2] << 8 | ud[i + 3];
                out->concat_total = ud[i + 4];
                out->concat_seq = ud[i + 5];
            }
            i += 2 + iel;
        }
        if (out->concat && (out->concat_total == 0 || out->concat_seq == 0 || out->concat_seq > out->concat_total))
            out->concat = false;
    }

    size_t o = 0;
    out->text[0] = 0;
    if (alphabet == 0) {
        int skip = udh_len ? (udh_len * 8 + 6) / 7 : 0;   // septets taken by the UDH + fill bits
        bool esc = false;
        for (int i = skip; i < udl; i++) {
            int bit = i * 7, byte = bit / 8, sh = bit % 8;
            if (byte >= ud_bytes) break;
            int v = ud[byte] >> sh;
            if (sh > 1 && byte + 1 < ud_bytes) v |= ud[byte + 1] << (8 - sh);
            v &= 0x7F;
            if (esc) {
                uint32_t cp = ' ';
                for (size_t k = 0; k < N_EXT; k++) if (GSM7_EXT[k].code == v) cp = GSM7_EXT[k].uc;
                o = utf8_put(out->text, o, sizeof(out->text), cp);
                esc = false;
            } else if (v == 0x1B) esc = true;
            else o = utf8_put(out->text, o, sizeof(out->text), GSM7[v]);
        }
    } else if (alphabet == 2) {
        int end = udl < ud_bytes ? udl : ud_bytes;
        for (int i = udh_len; i + 1 < end; i += 2) {
            uint32_t cp = ud[i] << 8 | ud[i + 1];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < end) {
                uint32_t lo = ud[i + 2] << 8 | ud[i + 3];
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            o = utf8_put(out->text, o, sizeof(out->text), cp);
        }
    } else {
        out->binary = true;
        int end = udl < ud_bytes ? udl : ud_bytes;
        int pos = 0;
        for (int i = udh_len; i < end && pos + 3 < (int)sizeof(out->text); i++) put_hex(out->text, &pos, ud[i]);
    }
    return 0;
}

// ---------- encode ----------
typedef struct { uint8_t code; bool ext; } g7_t;

// Converts to GSM-7 codes. Returns the number of characters (each 1 or 2 septets), -1 if not GSM-7.
static int to_gsm7(const char *s, g7_t *out, int cap)
{
    int n = 0, used;
    uint32_t cp;
    while ((used = utf8_next(s, &cp)) > 0) {
        g7_t g;
        if (!gsm7_lookup(cp, &g.code, &g.ext)) return -1;
        if (n >= cap) return -2;
        out[n++] = g;
        s += used;
    }
    return *s ? -1 : n;
}

// UTF-16 code units. Returns the count, -1 if invalid UTF-8, -2 if too long.
static int to_ucs2(const char *s, uint16_t *out, int cap)
{
    int n = 0, used;
    uint32_t cp;
    while ((used = utf8_next(s, &cp)) > 0) {
        if (cp >= 0x10000) {
            if (n + 2 > cap) return -2;
            cp -= 0x10000;
            out[n++] = 0xD800 | (cp >> 10);
            out[n++] = 0xDC00 | (cp & 0x3FF);
        } else {
            if (n + 1 > cap) return -2;
            out[n++] = cp;
        }
        s += used;
    }
    return *s ? -1 : n;
}

#define MAX_UNITS (PDU_MAX_PARTS * 160)
typedef struct { int start, end; } span_t;

// Splits into parts. GSM-7: 160 septets single / 153 per part, never splitting an escape pair.
// UCS-2: 70 units single / 67 per part, never splitting a surrogate pair.
static int split(const g7_t *g, const uint16_t *u, int n, bool ucs2, span_t *spans, int max_parts)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += ucs2 ? 1 : (g[i].ext ? 2 : 1);
    int single = ucs2 ? 70 : 160, multi = ucs2 ? 67 : 153;
    if (total <= single) { spans[0].start = 0; spans[0].end = n; return 1; }
    int parts = 0, i = 0;
    while (i < n) {
        if (parts == max_parts) return -1;
        int start = i, used = 0;
        while (i < n) {
            int w = ucs2 ? ((u[i] >= 0xD800 && u[i] < 0xDC00) ? 2 : 1) : (g[i].ext ? 2 : 1);
            if (used + w > multi) break;
            used += w;
            i += (ucs2 && w == 2) ? 2 : 1;
        }
        spans[parts].start = start;
        spans[parts].end = i;
        parts++;
    }
    return parts;
}

// Work buffers are on the heap so several tasks can encode at once.
typedef struct { g7_t g[MAX_UNITS]; uint16_t u[MAX_UNITS]; } work_t;

int pdu_count_parts(const char *utf8, bool *ucs2)
{
    work_t *w = malloc(sizeof(work_t));
    if (!w) return -1;
    span_t spans[PDU_MAX_PARTS];
    int n = to_gsm7(utf8, w->g, MAX_UNITS);
    bool uc = n == -1;
    if (uc) n = to_ucs2(utf8, w->u, MAX_UNITS);
    int r = n < 0 ? -1 : split(w->g, w->u, n, uc, spans, PDU_MAX_PARTS);
    if (ucs2) *ucs2 = uc;
    free(w);
    return r;
}

int pdu_encode(const char *to, const char *utf8, uint8_t ref, pdu_part_t *parts, int max_parts)
{
    span_t spans[PDU_MAX_PARTS];
    if (max_parts > PDU_MAX_PARTS) max_parts = PDU_MAX_PARTS;

    // Destination address
    const char *num = to;
    uint8_t toa = 0x81;                          // unknown / ISDN
    if (*num == '+') { toa = 0x91; num++; }
    int ndig = strlen(num);
    if (ndig == 0 || ndig > 20) return -1;
    for (int i = 0; i < ndig; i++) if (!isdigit((unsigned char)num[i])) return -1;

    work_t *w = malloc(sizeof(work_t));
    if (!w) return -1;
    const g7_t *g = w->g;
    const uint16_t *u = w->u;
    int n = to_gsm7(utf8, w->g, MAX_UNITS);
    bool ucs2 = n == -1;
    if (ucs2) n = to_ucs2(utf8, w->u, MAX_UNITS);
    int np = n < 0 ? -1 : split(g, u, n, ucs2, spans, max_parts);
    if (np < 0) { free(w); return -1; }

    for (int k = 0; k < np; k++) {
        char *h = parts[k].hex;
        int pos = 0;
        h[0] = 0;
        put_hex(h, &pos, 0x00);                  // use the SMSC stored on the SIM
        put_hex(h, &pos, 0x01 | (np > 1 ? 0x40 : 0));   // SMS-SUBMIT, UDHI when concatenated
        put_hex(h, &pos, 0x00);                  // message reference: chosen by the modem
        put_hex(h, &pos, ndig);
        put_hex(h, &pos, toa);
        for (int i = 0; i < ndig; i += 2) {
            uint8_t lo = num[i] - '0', hi = (i + 1 < ndig) ? num[i + 1] - '0' : 0xF;
            put_hex(h, &pos, hi << 4 | lo);
        }
        put_hex(h, &pos, 0x00);                  // PID
        put_hex(h, &pos, ucs2 ? 0x08 : 0x00);    // DCS

        uint8_t ud[140];
        int udl, ud_len = 0;
        uint8_t udh[6] = { 5, 0x00, 3, ref, (uint8_t)np, (uint8_t)(k + 1) };
        int hl = np > 1 ? 6 : 0;
        memcpy(ud, udh, hl);
        if (ucs2) {
            ud_len = hl;
            for (int i = spans[k].start; i < spans[k].end; i++) {
                ud[ud_len++] = u[i] >> 8;
                ud[ud_len++] = u[i] & 0xFF;
            }
            udl = ud_len;
        } else {
            // septet stream, starting after the UDH (6 bytes = 48 bits -> 7 septets, 1 fill bit)
            int sept0 = hl ? 7 : 0;
            int s = sept0;
            memset(ud + hl, 0, sizeof(ud) - hl);
            for (int i = spans[k].start; i < spans[k].end; i++) {
                uint8_t codes[2] = { 0x1B, g[i].code };
                int cnt = g[i].ext ? 2 : 1;
                for (int c = 0; c < cnt; c++) {
                    uint8_t v = g[i].ext ? codes[c] : g[i].code;
                    int bit = s * 7, byte = bit / 8, sh = bit % 8;
                    ud[byte] |= v << sh;
                    if (sh > 1) ud[byte + 1] |= v >> (8 - sh);
                    s++;
                }
            }
            udl = s;
            ud_len = (s * 7 + 7) / 8;
            if (ud_len < hl) ud_len = hl;
        }
        put_hex(h, &pos, udl);
        for (int i = 0; i < ud_len; i++) put_hex(h, &pos, ud[i]);
        parts[k].tpdu_len = pos / 2 - 1;
    }
    free(w);
    return np;
}
