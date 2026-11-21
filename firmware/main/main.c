// LTE SMS + voice gateway firmware, rev0.6 board.
// Step 1: WiFi + Tailscale (microlink), modem power + status, HTTP API on the tailnet, setup mode.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "app.h"
#include "board.h"
#include "settings.h"
#include "modem.h"
#include "net.h"
#include "api.h"
#include "console.h"
#include "setup.h"
#include "sms.h"
#include "sms_store.h"
#include "calls.h"
#include "events.h"
#include "voice.h"
#include "ota.h"
#include "peers.h"
#include "logbuf.h"

static const char *TAG = "gw";

// SMS engine -> WebSocket events. sms_received carries the whole message; the server still
// has to DELETE /sms/inbox/<id> once it has stored it.
static void on_sms_event(const char *ev, uint32_t id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", id);
    if (strcmp(ev, "sms_received") == 0) {
        sms_in_t m;
        if (sms_inbox_get(id, &m) == ESP_OK) {
            cJSON_AddStringToObject(o, "from", m.from);
            cJSON_AddStringToObject(o, "timestamp", m.timestamp);
            cJSON_AddStringToObject(o, "text", m.text);
            cJSON_AddNumberToObject(o, "parts", m.parts);
            cJSON_AddBoolToObject(o, "complete", m.complete);
            sms_in_free(&m);
        }
    } else {
        sms_out_t m;
        if (sms_outbox_get(id, &m) == ESP_OK) {
            cJSON_AddStringToObject(o, "to", m.to);
            cJSON_AddNumberToObject(o, "parts", m.parts);
            if (m.error[0]) cJSON_AddStringToObject(o, "error", m.error);
            sms_out_free(&m);
        }
    }
    events_publish(ev, o);
}

static void set_setup_flag(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("gw_sys", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "setup", v);
    nvs_commit(h);
    nvs_close(h);
}

static bool take_setup_flag(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("gw_sys", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "setup", &v);
    if (v) { nvs_erase_key(h, "setup"); nvs_commit(h); }
    nvs_close(h);
    return v != 0;
}

// Audio follows the call: relay while active, stop otherwise.
static void on_call(const call_info_t *c)
{
    if (c->state == CALL_ACTIVE) voice_start();
    else voice_stop();
}

void gw_restart(void)
{
    ESP_LOGW(TAG, "Restarting");
    net_stop();
    modem_power_off();
    esp_restart();
}

void gw_restart_into_setup(void)
{
    set_setup_flag(1);
    gw_restart();
}

// Hold BOOT (SW202) for 5 s at any time to restart into setup mode.
static void button_task(void *arg)
{
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_BOOT_BUTTON, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    int held = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held = gpio_get_level(PIN_BOOT_BUTTON) ? 0 : held + 1;
        if (held == 50) {
            ESP_LOGW(TAG, "BOOT held 5 s: restarting into setup mode");
            gw_restart_into_setup();
        }
    }
}

void app_main(void)
{
    board_early_init();               // PWRKEY low before anything else
    logbuf_init();                    // copy of the log for GET /logs
    ESP_ERROR_CHECK(settings_init());
    ota_boot_check();                 // new firmware: rolled back unless it reaches the tailnet
    peers_init();
    board_adc_init();
    modem_init();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 1);
    console_start();

    char ssid[33];
    bool setup = take_setup_flag() || !settings_get("wifi_ssid", ssid, sizeof(ssid));
    printf("\nLTE gateway %s. Type 'help' on this console.\n", GW_VERSION);
    if (setup) {
        ota_mark_valid("setup mode"); // the tailnet can't be checked here
        setup_mode_run();             // never returns
    }
    modem_start();
    sms_start();
    sms_set_event_cb(on_sms_event);
    voice_init();
    calls_set_cb(on_call);
    calls_start();
    net_start();
    api_start();
}
