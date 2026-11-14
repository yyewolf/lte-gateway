// G.711 µ-law / A-law (ITU-T G.711), pure C so it can be tested on a PC.
#include "g711.h"

uint8_t g711_lin2ulaw(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (pcm >> 8) & 0x80;
    int s = sign ? -(int)pcm : pcm;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    int exp = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) exp--;
    int mant = (s >> (exp + 3)) & 0x0F;
    return ~(sign | (exp << 4) | mant);
}

int16_t g711_ulaw2lin(uint8_t u)
{
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

int16_t g711_alaw2lin(uint8_t a)
{
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else t = (t + 0x108) << (seg - 1);
    return (a & 0x80) ? t : -t;
}
