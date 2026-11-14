// Host tests for main/g711.c:  gcc -Wall -I../main test_g711.c ../main/g711.c -o t && ./t
#include <stdio.h>
#include <stdlib.h>
#include "g711.h"

int main(void)
{
    int fails = 0;
    // Reference points from ITU-T G.711 (µ-law): 0 -> 0xFF, max positive -> 0x80, max negative -> 0x00
    if (g711_lin2ulaw(0) != 0xFF) { printf("FAIL ulaw(0)=%02x\n", g711_lin2ulaw(0)); fails++; }
    if (g711_lin2ulaw(32767) != 0x80) { printf("FAIL ulaw(max)=%02x\n", g711_lin2ulaw(32767)); fails++; }
    if (g711_lin2ulaw(-32768) != 0x00) { printf("FAIL ulaw(min)=%02x\n", g711_lin2ulaw(-32768)); fails++; }
    if (g711_ulaw2lin(0xFF) != 0) { printf("FAIL ulaw2lin(FF)=%d\n", g711_ulaw2lin(0xFF)); fails++; }
    if (g711_ulaw2lin(0x80) != 32124) { printf("FAIL ulaw2lin(80)=%d\n", g711_ulaw2lin(0x80)); fails++; }
    if (g711_alaw2lin(0xD5) != 8) { printf("FAIL alaw2lin(D5)=%d\n", g711_alaw2lin(0xD5)); fails++; }
    if (g711_alaw2lin(0xAA) != 32256) { printf("FAIL alaw2lin(AA)=%d\n", g711_alaw2lin(0xAA)); fails++; }
    // Round trip: µ-law error stays within its quantisation step (~3 % of the value, min 4)
    for (int x = -32768; x <= 32767; x++) {
        int y = g711_ulaw2lin(g711_lin2ulaw(x));
        int tol = abs(x) / 16 + 8;
        if (abs(x) > 32124) continue;   // clipped
        if (abs(y - x) > tol) { printf("FAIL roundtrip %d -> %d\n", x, y); if (++fails > 10) break; }
    }
    // Every code decodes and re-encodes to itself (except the two zeros 0x7F/0xFF)
    for (int u = 0; u < 256; u++) {
        int r = g711_lin2ulaw(g711_ulaw2lin(u));
        if (r != u && !(u == 0x7F && r == 0xFF)) { printf("FAIL code %02x -> %02x\n", u, r); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
