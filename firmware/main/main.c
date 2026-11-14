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

static const char *TAG = "gw";

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

void gw_restart(void)
{
    ESP_LOGW(TAG, "Restarting");
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
    ESP_ERROR_CHECK(settings_init());
    board_adc_init();
    modem_init();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 1);
    console_start();

    char ssid[33];
    bool setup = take_setup_flag() || !settings_get("wifi_ssid", ssid, sizeof(ssid));
    printf("\nLTE gateway %s. Type 'help' on this console.\n", GW_VERSION);
    if (setup) {
        setup_mode_run();             // never returns
    }
    modem_start();
    sms_start();
    net_start();
    api_start();
}
