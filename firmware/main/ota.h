// Firmware updates over the tailnet: POST the app image (build/lte_gateway.bin) to /ota.
// A/B app slots (ota_0 / ota_1). A new image boots "pending": it is kept once it reaches the
// tailnet, so the next update can still get in. If it crashes or can't reach the tailnet
// within OTA_CONFIRM_TIMEOUT_S, the bootloader goes back to the previous image.
#pragma once
#include "esp_http_server.h"
#include "cJSON.h"

#define OTA_CONFIRM_TIMEOUT_S 300

void ota_boot_check(void);              // early in app_main: log the slot, arm the rollback timer
void ota_mark_valid(const char *why);   // keep the running image (no-op unless pending)
void ota_register(httpd_handle_t hd);   // GET /ota, POST /ota, POST /ota/rollback
cJSON *ota_status_json(void);
