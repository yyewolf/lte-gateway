#include "peers.h"
#include "net.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "microlink.h"

static const char *TAG = "peers";

#define NS "gw_peers"
#define KEY "pinned"

typedef struct {
    uint32_t ip;          // tailnet IP, host byte order
    char name[64];        // hostname when it was pinned (for display while the peer is away)
} pin_t;

static pin_t s_pins[PEERS_MAX_PINNED];
static int s_count;
static SemaphoreHandle_t s_lock;
static microlink_t *s_ml;
static microlink_peer_info_t s_dropped[16];   // scratch, only used under s_lock

_Static_assert(PEERS_MAX_PINNED == MICROLINK_MAX_PINNED, "pinned list sizes differ");

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = s_count ? nvs_set_blob(h, KEY, s_pins, s_count * sizeof(pin_t)) : nvs_erase_key(h, KEY);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// Pushes the current list to microlink. Caller holds the lock.
static void push(void)
{
    if (!s_ml) return;
    uint32_t ips[PEERS_MAX_PINNED];
    for (int i = 0; i < s_count; i++) ips[i] = s_pins[i].ip;
    esp_err_t e = microlink_set_pinned_peers(s_ml, ips, s_count);
    if (e != ESP_OK) ESP_LOGW(TAG, "microlink_set_pinned_peers: %s", esp_err_to_name(e));
}

void peers_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_pins);
    if (nvs_get_blob(h, KEY, s_pins, &len) == ESP_OK && len % sizeof(pin_t) == 0) s_count = len / sizeof(pin_t);
    nvs_close(h);
    for (int i = 0; i < s_count; i++) s_pins[i].name[sizeof(s_pins[i].name) - 1] = 0;
    ESP_LOGI(TAG, "%d pinned peer(s)", s_count);
}

void peers_apply(microlink_t *ml)
{
    lock();
    s_ml = ml;
    push();
    unlock();
}

// Hostname of a peer the tailnet told us about (in the table or refused), "" if unknown.
// Caller holds the lock.
static void lookup_name(uint32_t ip, char *out, size_t len)
{
    out[0] = 0;
    if (!s_ml) return;
    microlink_peer_info_t p;
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        memset(&p, 0, sizeof(p));
        if (microlink_get_peer_info(s_ml, i, &p) == ESP_OK && p.online && p.vpn_ip == ip) {
            strlcpy(out, p.hostname, len);
            return;
        }
    }
    n = microlink_get_dropped_peers(s_ml, s_dropped, 16);
    for (int i = 0; i < n; i++)
        if (s_dropped[i].vpn_ip == ip) { strlcpy(out, s_dropped[i].hostname, len); return; }
}

static int find(uint32_t ip)
{
    for (int i = 0; i < s_count; i++)
        if (s_pins[i].ip == ip) return i;
    return -1;
}

esp_err_t peers_pin(uint32_t ip, const char **why)
{
    if ((ip & 0xFFC00000u) != 0x64400000u) { *why = "not a tailnet address (100.64.0.0/10)"; return ESP_ERR_INVALID_ARG; }
    lock();
    if (find(ip) >= 0) { unlock(); return ESP_OK; }
    if (s_count >= PEERS_MAX_PINNED) { unlock(); *why = "pinned list full (8): unpin one first"; return ESP_ERR_NO_MEM; }
    pin_t *p = &s_pins[s_count++];
    memset(p, 0, sizeof(*p));
    p->ip = ip;
    lookup_name(ip, p->name, sizeof(p->name));
    esp_err_t e = save();
    if (e != ESP_OK) { s_count--; unlock(); *why = "could not save to flash"; return e; }
    push();
    char s[16];
    microlink_ip_to_str(ip, s);
    ESP_LOGI(TAG, "Pinned %s %s", s, p->name);
    unlock();
    return ESP_OK;
}

esp_err_t peers_unpin(uint32_t ip, const char **why)
{
    lock();
    int i = find(ip);
    if (i < 0) { unlock(); *why = "not pinned"; return ESP_ERR_NOT_FOUND; }
    pin_t old = s_pins[i];
    memmove(&s_pins[i], &s_pins[i + 1], (s_count - i - 1) * sizeof(pin_t));
    s_count--;
    esp_err_t e = save();
    if (e != ESP_OK) {
        memmove(&s_pins[i + 1], &s_pins[i], (s_count - i) * sizeof(pin_t));
        s_pins[i] = old;
        s_count++;
        unlock();
        *why = "could not save to flash";
        return e;
    }
    push();
    unlock();
    return ESP_OK;
}

static uint32_t setting_prio(void)
{
    char v[20];
    return settings_get("priority_peer", v, sizeof(v)) ? microlink_parse_ip(v) : 0;
}

cJSON *peers_json(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "peers");
    cJSON *drop = cJSON_AddArrayToObject(r, "dropped");
    cJSON *pins = cJSON_AddArrayToObject(r, "pinned");
    uint32_t prio = setting_prio();
    char ip[16];

    lock();
    if (s_ml) {
        int n = microlink_get_peer_count(s_ml);
        for (int i = 0; i < n; i++) {
            microlink_peer_info_t p;
            memset(&p, 0, sizeof(p));
            if (microlink_get_peer_info(s_ml, i, &p) != ESP_OK || !p.online) continue;   // free slot
            p.hostname[sizeof(p.hostname) - 1] = 0;
            microlink_ip_to_str(p.vpn_ip, ip);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddStringToObject(o, "hostname", p.hostname);
            cJSON_AddStringToObject(o, "path", p.direct_path ? "direct" : "derp");
            cJSON_AddBoolToObject(o, "pinned", find(p.vpn_ip) >= 0);
            cJSON_AddBoolToObject(o, "priority_setting", p.vpn_ip == prio);
            cJSON_AddItemToArray(arr, o);
        }
        microlink_peer_info_t *d = s_dropped;
        n = microlink_get_dropped_peers(s_ml, d, 16);
        for (int i = n - 1; i >= 0; i--) {       // newest first
            d[i].hostname[sizeof(d[i].hostname) - 1] = 0;
            microlink_ip_to_str(d[i].vpn_ip, ip);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddStringToObject(o, "hostname", d[i].hostname);
            cJSON_AddBoolToObject(o, "pinned", find(d[i].vpn_ip) >= 0);
            cJSON_AddItemToArray(drop, o);
        }
    }
    for (int i = 0; i < s_count; i++) {
        microlink_ip_to_str(s_pins[i].ip, ip);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddStringToObject(o, "hostname", s_pins[i].name);
        cJSON_AddItemToArray(pins, o);
    }
    unlock();

    if (prio) {
        microlink_ip_to_str(prio, ip);
        cJSON_AddStringToObject(r, "priority_peer", ip);
    } else {
        cJSON_AddNullToObject(r, "priority_peer");
    }
    cJSON_AddNumberToObject(r, "max_peers", CONFIG_ML_MAX_PEERS);
    cJSON_AddNumberToObject(r, "max_pinned", PEERS_MAX_PINNED);
    return r;
}

void peers_print(void)
{
    cJSON *j = peers_json();
    const cJSON *it;
    int n = 0;
    printf("In the tunnel table (max %d):\n", CONFIG_ML_MAX_PEERS);
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "peers")) {
        bool pin = cJSON_IsTrue(cJSON_GetObjectItem(it, "pinned"));
        bool prio = cJSON_IsTrue(cJSON_GetObjectItem(it, "priority_setting"));
        printf("  %-15s %-32s %-6s %s\n", cJSON_GetObjectItem(it, "ip")->valuestring,
               cJSON_GetObjectItem(it, "hostname")->valuestring, cJSON_GetObjectItem(it, "path")->valuestring,
               pin ? "pinned" : prio ? "priority_peer" : "");
        n++;
    }
    if (!n) printf("  (none)\n");
    n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "dropped")) {
        if (!n++) printf("No slot (table full), cannot reach the gateway:\n");
        printf("  %-15s %-32s %s\n", cJSON_GetObjectItem(it, "ip")->valuestring,
               cJSON_GetObjectItem(it, "hostname")->valuestring,
               cJSON_IsTrue(cJSON_GetObjectItem(it, "pinned")) ? "pinned" : "");
    }
    n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "pinned")) {
        if (!n++) printf("Pinned:\n");
        printf("  %-15s %s\n", cJSON_GetObjectItem(it, "ip")->valuestring, cJSON_GetObjectItem(it, "hostname")->valuestring);
    }
    if (!n) printf("Pinned: none ('pin <100.x.y.z>')\n");
    cJSON_Delete(j);
}
