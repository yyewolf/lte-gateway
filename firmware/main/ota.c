#include "ota.h"
#include "api.h"
#include "app.h"
#include "calls.h"
#include "events.h"
#include "settings.h"
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

static const char *TAG = "ota";

static bool s_pending;                 // running image not confirmed yet
static esp_timer_handle_t s_confirm_timer;
static bool s_restarting;

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static void restart_task(void *arg)
{
    bool rollback = (bool)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));          // let the HTTP response and the event go out
    if (rollback) {
        // Marks this image invalid and points the boot slot back at the previous one.
        esp_err_t e = esp_ota_mark_app_invalid_rollback();
        if (e != ESP_OK) ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(e));
    }
    gw_restart();
}

static void schedule_restart(bool rollback)
{
    if (s_restarting) return;
    s_restarting = true;
    xTaskCreatePinnedToCore(restart_task, "ota_restart", 4096, (void *)(intptr_t)rollback, 5, NULL, 1);
}

static void confirm_timeout(void *arg)
{
    if (!s_pending) return;
    ESP_LOGE(TAG, "New firmware did not reach the tailnet within %d s: rolling back", OTA_CONFIRM_TIMEOUT_S);
    schedule_restart(true);
}

void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "Running %s from %s (%s)", esp_app_get_description()->version, run->label, state_name(st));
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) {
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(bad, &d) == ESP_OK)
            ESP_LOGW(TAG, "Rolled back: %s in %s was refused", d.version, bad->label);
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
    s_pending = true;
    ESP_LOGW(TAG, "New firmware: kept once the tailnet is up, rolled back otherwise (%d s)", OTA_CONFIRM_TIMEOUT_S);
    const esp_timer_create_args_t ta = { .callback = confirm_timeout, .name = "ota_confirm" };
    if (esp_timer_create(&ta, &s_confirm_timer) == ESP_OK)
        esp_timer_start_once(s_confirm_timer, (uint64_t)OTA_CONFIRM_TIMEOUT_S * 1000000);
}

void ota_mark_valid(const char *why)
{
    if (!s_pending || s_restarting) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending = false;
        if (s_confirm_timer) esp_timer_stop(s_confirm_timer);
        ESP_LOGI(TAG, "Firmware %s confirmed (%s)", esp_app_get_description()->version, why);
    }
}

static cJSON *slot_json(const esp_partition_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "partition", p->label);
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) cJSON_AddStringToObject(o, "version", d.version);
    else cJSON_AddNullToObject(o, "version");
    esp_ota_img_states_t st;
    cJSON_AddStringToObject(o, "state", esp_ota_get_state_partition(p, &st) == ESP_OK ? state_name(st) : "undefined");
    return o;
}

cJSON *ota_status_json(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "running", slot_json(run));
    if (other) cJSON_AddItemToObject(r, "other", slot_json(other));
    cJSON_AddNumberToObject(r, "max_image_size", other ? other->size : 0);
    cJSON_AddBoolToObject(r, "pending_verify", s_pending);
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(r, "rollback_enabled", true);
#else
    cJSON_AddBoolToObject(r, "rollback_enabled", false);
#endif
    return r;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return r;
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, status, o);
}

static bool query_flag(httpd_req_t *req, const char *key)
{
    char q[48], v[8];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK && (v[0] == '1' || v[0] == 't');
}

// Common checks for anything that reboots: token set, no call unless ?force=1.
static bool may_reboot(httpd_req_t *req)
{
    char tok[8];
    if (!settings_get("api_token", tok, sizeof(tok))) {
        send_err(req, "403 Forbidden", "set api_token first (console: set api_token <secret>), "
                                       "firmware updates need it");
        return false;
    }
    if (s_restarting) {
        send_err(req, "409 Conflict", "already restarting");
        return false;
    }
    call_info_t c;
    calls_get(&c);
    if (c.state != CALL_IDLE && !query_flag(req, "force")) {
        send_err(req, "409 Conflict", "a call is in progress (add ?force=1 to cut it)");
        return false;
    }
    return true;
}

static esp_err_t h_ota_get(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    return send_json(req, NULL, ota_status_json());
}

#define HDR_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define CHUNK 4096

// Checks the start of the upload before anything is erased: an ESP32-S3 app image of this project.
static const char *check_header(const uint8_t *buf, esp_app_desc_t *out)
{
    const esp_image_header_t *h = (const esp_image_header_t *)buf;
    if (h->magic != ESP_IMAGE_HEADER_MAGIC) return "not an ESP-IDF app image (send build/lte_gateway.bin, not the merged or bootloader .bin)";
    if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return "image is for another chip";
    memcpy(out, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(*out));
    if (out->magic_word != ESP_APP_DESC_MAGIC_WORD) return "image has no app description";
    out->project_name[sizeof(out->project_name) - 1] = 0;
    out->version[sizeof(out->version) - 1] = 0;
    if (strncmp(out->project_name, esp_app_get_description()->project_name, sizeof(out->project_name)) != 0)
        return "image is another project (not lte_gateway)";
    return NULL;
}

// POST /ota   body = build/lte_gateway.bin (application/octet-stream)
static esp_err_t h_ota_post(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    if (!may_reboot(req)) return ESP_OK;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_err(req, "500 Internal Server Error", "no OTA partition");
    size_t total = req->content_len;
    if (total < HDR_LEN) return send_err(req, "400 Bad Request", "body missing or too small (send the .bin as the body)");
    if (total > part->size) return send_err(req, "413 Payload Too Large", "image larger than the app slot");

    uint8_t *buf = malloc(CHUNK);
    if (!buf) return send_err(req, "500 Internal Server Error", "out of memory");

    esp_ota_handle_t h = 0;
    bool begun = false;
    const char *why = NULL;
    const char *status = "400 Bad Request";
    esp_app_desc_t desc;
    size_t got = 0, have = 0;
    int timeouts = 0, last_pct = -10;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)total, part->label);

    while (got < total) {
        int r = httpd_req_recv(req, (char *)buf + have, MIN(CHUNK - have, total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 6) { why = "upload stalled"; status = "408 Request Timeout"; break; }
            continue;
        }
        if (r <= 0) { why = "connection closed during upload"; break; }
        timeouts = 0;
        got += r;
        have += r;
        if (!begun) {
            if (have < HDR_LEN) continue;
            if ((why = check_header(buf, &desc)) != NULL) break;
            esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (e != ESP_OK) { why = esp_err_to_name(e); status = "500 Internal Server Error"; break; }
            begun = true;
            ESP_LOGI(TAG, "Image %s %s (%s %s)", desc.project_name, desc.version, desc.date, desc.time);
        }
        if (have == CHUNK || got == total) {
            esp_err_t e = esp_ota_write(h, buf, have);
            if (e != ESP_OK) { why = esp_err_to_name(e); status = "500 Internal Server Error"; break; }
            have = 0;
        }
        int pct = (int)(got * 100 / total);
        if (pct >= last_pct + 10) { ESP_LOGI(TAG, "%d%%", pct); last_pct = pct; }
    }
    free(buf);

    if (!why && begun) {
        esp_err_t e = esp_ota_end(h);
        begun = false;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) why = "image failed verification (corrupt or truncated)";
        else if (e != ESP_OK) { why = esp_err_to_name(e); status = "500 Internal Server Error"; }
        else if ((e = esp_ota_set_boot_partition(part)) != ESP_OK) { why = esp_err_to_name(e); status = "500 Internal Server Error"; }
    }
    if (begun) esp_ota_abort(h);
    if (why) {
        ESP_LOGE(TAG, "Update refused: %s", why);
        return send_err(req, status, why);
    }

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, "Firmware %s written to %s in %d ms, restarting", desc.version, part->label, ms);
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "state", "restarting");
    cJSON_AddStringToObject(ev, "version", desc.version);
    events_publish("ota", ev);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", "ok");
    cJSON_AddStringToObject(o, "version", desc.version);
    cJSON_AddStringToObject(o, "previous", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "partition", part->label);
    cJSON_AddNumberToObject(o, "bytes", (double)total);
    cJSON_AddNumberToObject(o, "ms", ms);
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddStringToObject(o, "note", "restarting; the new firmware is kept once it reaches the tailnet");
#else
    cJSON_AddStringToObject(o, "note", "restarting (rollback is off in this build)");
#endif
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t r = send_json(req, NULL, o);
    schedule_restart(false);
    return r;
}

// POST /ota/rollback: boot the other slot's firmware (the previous one).
static esp_err_t h_ota_rollback(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    if (!may_reboot(req)) return ESP_OK;
    if (s_pending) {
        // Not confirmed yet: the bootloader still has the previous image marked valid.
        s_pending = false;
        if (s_confirm_timer) esp_timer_stop(s_confirm_timer);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "result", "rolling back");
        esp_err_t r = send_json(req, NULL, o);
        schedule_restart(true);
        return r;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK)
        return send_err(req, "409 Conflict", "no firmware in the other slot");
    esp_ota_get_state_partition(other, &st);
    if (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)
        return send_err(req, "409 Conflict", "the other slot's firmware failed before and was refused");
    esp_err_t e = esp_ota_set_boot_partition(other);     // verifies the image
    if (e != ESP_OK) return send_err(req, "409 Conflict", "the other slot's image does not verify");
    ESP_LOGW(TAG, "Switching back to %s in %s", d.version, other->label);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", "restarting");
    cJSON_AddStringToObject(o, "version", d.version);
    cJSON_AddStringToObject(o, "partition", other->label);
    esp_err_t r = send_json(req, NULL, o);
    schedule_restart(false);
    return r;
}

void ota_register(httpd_handle_t hd)
{
    const httpd_uri_t uris[] = {
        { .uri = "/ota", .method = HTTP_GET, .handler = h_ota_get },
        { .uri = "/ota", .method = HTTP_POST, .handler = h_ota_post },
        { .uri = "/ota/rollback", .method = HTTP_POST, .handler = h_ota_rollback },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(hd, &uris[i]);
}
