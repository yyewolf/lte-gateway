#include "settings.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define NS "gw"

const setting_def_t g_settings[] = {
    { "wifi_ssid",     "WiFi network name", false },
    { "wifi_pass",     "WiFi password", true },
    { "ts_authkey",    "Tailscale auth key (tskey-auth-...), only needed for the first login", true },
    { "hostname",      "Name on the tailnet (default lte-gateway)", false },
    { "priority_peer", "Tailnet IP of your server (100.x.y.z): always keeps a tunnel slot", false },
    { "api_token",     "Optional: require 'Authorization: Bearer <token>' on the API", true },
};
const size_t g_settings_count = sizeof(g_settings) / sizeof(g_settings[0]);

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

const setting_def_t *settings_find(const char *key)
{
    for (size_t i = 0; i < g_settings_count; i++)
        if (strcmp(g_settings[i].key, key) == 0) return &g_settings[i];
    return NULL;
}

bool settings_get(const char *key, char *out, size_t len)
{
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    esp_err_t err = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    if (err != ESP_OK) out[0] = 0;
    return out[0] != 0;
}

esp_err_t settings_set(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (value && value[0]) err = nvs_set_str(h, key, value);
    else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
