#pragma once
#include <stdint.h>

uint8_t g711_lin2ulaw(int16_t pcm);
int16_t g711_ulaw2lin(uint8_t u);
int16_t g711_alaw2lin(uint8_t a);
