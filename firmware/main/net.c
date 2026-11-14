#include "net.h"
#include "settings.h"
#include "board.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "microlink.h"

static const char *TAG = "net";

static EventGroupHandle_t s_ev;
#define WIFI_UP BIT0
static microlink_t *s_ml;
static esp_netif_t *s_sta;
static char s_ssid[33];

static const char *state_name(microlink_state_t s)
{
    static const char *names[] = { "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING", "CONNECTED",
                                   "RECONNECTING", "ERROR" };
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d), retrying", d->reason);
        xEventGroupClearBits(s_ev, WIFI_UP);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "WiFi up on %s, LAN IP " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, WIFI_UP);
    }
}

static void on_ml_state(microlink_t *ml, microlink_state_t state, void *ud)
{
    ESP_LOGI(TAG, "Tailnet: %s", state_name(state));
    if (state == ML_STATE_CONNECTED) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(ml), ip);
        ESP_LOGI(TAG, "Tailnet IP %s: try  curl http://%s/status", ip, ip);
    }
    board_led(PIN_LED_RED, state != ML_STATE_CONNECTED);
}

static void net_task(void *arg)
{
    xEventGroupWaitBits(s_ev, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);

    static char authkey[128], hostname[64], prio[20];
    settings_get("ts_authkey", authkey, sizeof(authkey));
    if (!settings_get("hostname", hostname, sizeof(hostname))) strlcpy(hostname, "lte-gateway", sizeof(hostname));
    settings_get("priority_peer", prio, sizeof(prio));

    microlink_config_t cfg = {
        .auth_key = authkey,          // only used for the first login; the node key is kept in NVS
        .device_name = hostname,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 0,
        .priority_peer_ip = prio[0] ? microlink_parse_ip(prio) : 0,
    };
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        vTaskDelete(NULL);
    }
    microlink_set_state_callback(s_ml, on_ml_state, NULL);
    if (microlink_start(s_ml) != ESP_OK) ESP_LOGE(TAG, "microlink_start failed");
    vTaskDelete(NULL);
}

void net_start(void)
{
    static char pass[65];
    settings_get("wifi_ssid", s_ssid, sizeof(s_ssid));
    settings_get("wifi_pass", pass, sizeof(pass));
    s_ev = xEventGroupCreate();
    board_led(PIN_LED_RED, true);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save adds up to one DTIM interval (100-300 ms) per packet: bad for voice.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Connecting to WiFi \"%s\"...", s_ssid);

    xTaskCreatePinnedToCore(net_task, "net_start", 6144, NULL, 5, NULL, 1);
}

microlink_t *net_microlink(void) { return s_ml; }

uint32_t net_tailnet_ip(void)
{
    return (s_ml && microlink_is_connected(s_ml)) ? microlink_get_vpn_ip(s_ml) : 0;
}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    out->wifi_up = s_ev && (xEventGroupGetBits(s_ev) & WIFI_UP);
    if (out->wifi_up) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
        esp_netif_ip_info_t ip;
        if (s_sta && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK)
            snprintf(out->lan_ip, sizeof(out->lan_ip), IPSTR, IP2STR(&ip.ip));
    }
    out->tailnet_state = s_ml ? state_name(microlink_get_state(s_ml)) : "IDLE";
    out->tailnet_up = s_ml && microlink_is_connected(s_ml);
    if (out->tailnet_up) {
        microlink_ip_to_str(microlink_get_vpn_ip(s_ml), out->tailnet_ip);
        out->peers = microlink_get_peer_count(s_ml);
    }
}

void net_print_peers(void)
{
    if (!s_ml) { printf("Tailnet not started.\n"); return; }
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t p;
        if (microlink_get_peer_info(s_ml, i, &p) != ESP_OK) continue;
        char ip[16];
        microlink_ip_to_str(p.vpn_ip, ip);
        printf("  %-15s %-32s %s %s\n", ip, p.hostname, p.online ? "online " : "offline",
               p.direct_path ? "direct" : "relay (DERP)");
    }
    if (!n) printf("  (no peers)\n");
}
