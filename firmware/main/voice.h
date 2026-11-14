// Call audio: ES8311 codec <-> I2S <-> G.711 µ-law RTP over the tailnet (UDP), 20 ms frames,
// full duplex. The I2S loop runs on core 0 at high priority; network work stays on core 1.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define VOICE_RTP_PORT 4000     // local UDP port, and the default port on the server

esp_err_t voice_init(void);                       // codec + I2S, at boot
void voice_set_target(uint32_t ip, uint16_t port); // where RTP goes (host byte order), port 0 = default
void voice_start(void);                           // call became active
void voice_stop(void);                            // call ended
void voice_set_gains(float in_db, int out_vol);
cJSON *voice_stats_json(void);
