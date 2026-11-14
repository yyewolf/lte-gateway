// Host tests for main/pdu.c:  gcc -Wall -Wextra -I../main test_pdu.c ../main/pdu.c -o t && ./t
#include <stdio.h>
#include <string.h>
#include "pdu.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

// Encode, then decode each part as if it came back as a DELIVER: rewrite the SUBMIT header.
static void submit_to_deliver(const pdu_part_t *p, char *out)
{
    // SUBMIT: 00 | FO | MR | DA-len | TOA | DA... | PID | DCS | UDL | UD
    // DELIVER: 00 | FO | OA-len | TOA | OA... | PID | DCS | SCTS(7) | UDL | UD
    const char *h = p->hex;
    unsigned fo, dalen;
    sscanf(h + 2, "%2x", &fo);
    sscanf(h + 6, "%2x", &dalen);
    int addr_hex = 2 + 2 + ((dalen + 1) / 2) * 2;     // len + toa + digits
    char fo_d[3];
    sprintf(fo_d, "%02X", (fo & 0x40) | 0x04);        // DELIVER, keep UDHI
    out[0] = 0;
    strcat(out, "00");
    strcat(out, fo_d);
    strncat(out, h + 6, addr_hex);                    // address
    strncat(out, h + 6 + addr_hex, 4);                // PID + DCS
    strcat(out, "62114161553540");                    // 2026-11-14 16:55:53 +01:00 (tz 4 quarters)
    strcat(out, h + 6 + addr_hex + 4);                // UDL + UD
}

static void roundtrip(const char *text, int expect_parts, int expect_ucs2)
{
    pdu_part_t parts[PDU_MAX_PARTS];
    bool ucs2;
    int np = pdu_count_parts(text, &ucs2);
    CHECK(np == expect_parts);
    CHECK(ucs2 == expect_ucs2);
    int n = pdu_encode("+33612345678", text, 42, parts, PDU_MAX_PARTS);
    CHECK(n == expect_parts);
    if (n <= 0) return;
    char joined[4096] = "";
    for (int i = 0; i < n; i++) {
        CHECK((int)strlen(parts[i].hex) == 2 * (parts[i].tpdu_len + 1));
        CHECK(parts[i].tpdu_len <= 140 + 15);
        char d[PDU_HEX_MAX + 32];
        submit_to_deliver(&parts[i], d);
        pdu_deliver_t m;
        CHECK(pdu_decode(d, &m) == 0);
        CHECK_STR(m.sender, "+33612345678");
        CHECK_STR(m.timestamp, "2026-11-14T16:55:53+01:00");
        if (n > 1) { CHECK(m.concat); CHECK(m.concat_ref == 42); CHECK(m.concat_total == n); CHECK(m.concat_seq == i + 1); }
        else CHECK(!m.concat);
        strcat(joined, m.text);
    }
    CHECK_STR(joined, text);
}

int main(void)
{
    pdu_deliver_t m;

    // Classic SMS-DELIVER example: "How are you?" from +31641600986
    CHECK(pdu_decode("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", &m) == 0);
    CHECK_STR(m.sender, "+31641600986");
    CHECK_STR(m.text, "How are you?");
    CHECK(!m.concat);

    // Alphanumeric sender: "Free" = 4 septets = 28 bits = 4 bytes -> 7 semi-octets
    CHECK(pdu_decode("0004" "07D04679B90C" "0008" "62114161553540" "04" "00E90020", &m) == 0);
    CHECK_STR(m.sender, "Free");
    CHECK_STR(m.text, "\xC3\xA9 ");

    roundtrip("Hello", 1, 0);
    roundtrip("", 1, 0);
    roundtrip("Prix: 5\xE2\x82\xAC [promo] {ok} ~^|\\", 1, 0);          // GSM-7 extension chars
    roundtrip("Caf\xC3\xA9 \xC3\xA0 l'h\xC3\xB4tel", 1, 1);              // ô is not GSM-7 -> UCS-2
    roundtrip("Emoji \xF0\x9F\x98\x80 ok", 1, 1);                         // surrogate pair

    char longg[2000] = "";
    for (int i = 0; i < 160; i++) strcat(longg, "a");
    roundtrip(longg, 1, 0);                                               // exactly 160: one part
    strcat(longg, "b");
    roundtrip(longg, 2, 0);                                               // 161: two parts
    longg[0] = 0;
    for (int i = 0; i < 152; i++) strcat(longg, "x");
    strcat(longg, "\xE2\x82\xAC" "yyyyyyyyyy");                                // escape must not split at 153
    roundtrip(longg, 2, 0);

    char longu[4000] = "";
    for (int i = 0; i < 70; i++) strcat(longu, "\xC3\xB4");
    roundtrip(longu, 1, 1);
    strcat(longu, "\xC3\xB4");
    roundtrip(longu, 2, 1);
    longu[0] = 0;
    for (int i = 0; i < 66; i++) strcat(longu, "\xC3\xB4");
    strcat(longu, "\xF0\x9F\x98\x80" "zzzzzzzzzz");                                // surrogate at the 67 boundary
    roundtrip(longu, 2, 1);

    char huge[4000] = "";
    for (int i = 0; i < 153 * 10; i++) strcat(huge, "a");
    roundtrip(huge, 10, 0);
    strcat(huge, "a");
    pdu_part_t parts[PDU_MAX_PARTS];
    CHECK(pdu_encode("+33612345678", huge, 1, parts, PDU_MAX_PARTS) == -1);
    CHECK(pdu_encode("06 12", "x", 1, parts, PDU_MAX_PARTS) == -1);      // bad number
    CHECK(pdu_encode("0612345678", "x", 1, parts, PDU_MAX_PARTS) == 1);  // national number, TOA 81
    CHECK(strncmp(parts[0].hex, "0001000A81", 10) == 0);

    // Known SMS-SUBMIT: "hellohello" to +46708251358 (classic example)
    CHECK(pdu_encode("+46708251358", "hellohello", 0, parts, 1) == 1);
    CHECK_STR(parts[0].hex, "0001000B916407281553F800000AE8329BFD4697D9EC37");
    CHECK(parts[0].tpdu_len == 22);

    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
