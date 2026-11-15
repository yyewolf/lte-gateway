#include "events.h"
#include "api.h"
#include "app.h"
#include "sms_store.h"
#include "calls.h"
#include "voice.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "events";

#define MAX_CLIENTS 4
static httpd_handle_t s_hd;
static int s_fds[MAX_CLIENTS] = { -1, -1, -1, -1 };
static SemaphoreHandle_t s_lock;

typedef struct { char *json; int only_fd; httpd_ws_type_t type; } job_t;

// Runs in the HTTP server task, so sends never race with the server itself.
static void send_job(void *arg)
{
    job_t *j = arg;
    httpd_ws_frame_t f = { .type = j->type, .payload = (uint8_t *)j->json, .len = j->json ? strlen(j->json) : 0,
                           .final = true };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = s_fds[i];
        if (fd < 0 || (j->only_fd >= 0 && fd != j->only_fd)) continue;
        if (httpd_ws_get_fd_info(s_hd, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_frame_async(s_hd, fd, &f) != ESP_OK) {
            ESP_LOGI(TAG, "Client fd %d gone", fd);
            s_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_lock);
    free(j->json);
    free(j);
}

static void queue_send(char *json, int only_fd, httpd_ws_type_t type)
{
    job_t *j = malloc(sizeof(job_t));
    if (!j || !s_hd) { free(json); free(j); return; }
    j->json = json;
    j->only_fd = only_fd;
    j->type = type;
    if (httpd_queue_work(s_hd, send_job, j) != ESP_OK) { free(json); free(j); }
}

static char *build(const char *type, cJSON *fields)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddNumberToObject(o, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    while (fields && fields->child) {
        cJSON *it = cJSON_DetachItemViaPointer(fields, fields->child);
        cJSON_AddItemToObject(o, it->string, it);
    }
    cJSON_Delete(fields);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

void events_publish(const char *type, cJSON *fields)
{
    if (!s_hd || events_clients() == 0) { cJSON_Delete(fields); return; }
    char *s = build(type, fields);
    if (s) queue_send(s, -1, HTTPD_WS_TYPE_TEXT);
}

int events_clients(void)
{
    if (!s_lock) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] >= 0) n++;
    xSemaphoreGive(s_lock);
    return n;
}

static void send_hello(int fd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", GW_VERSION);
    cJSON_AddNumberToObject(o, "sms_inbox", sms_inbox_count());
    cJSON_AddItemToObject(o, "call", calls_state_json());
    char *s = build("hello", o);
    if (s) queue_send(s, fd, HTTPD_WS_TYPE_TEXT);
}

// IDF 6.x answers the WebSocket handshake itself and no longer calls the URI
// handler for it: the caller is checked in a pre-handshake callback and the
// client registered in a post-handshake callback. IDF 5.x calls the handler
// with the GET request instead.
static esp_err_t ws_check(httpd_req_t *req)
{
    return api_guard(req);
}

static esp_err_t ws_open(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_fds[i] < 0 || httpd_ws_get_fd_info(s_hd, s_fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) { slot = i; break; }
    if (slot >= 0) s_fds[slot] = fd;
    xSemaphoreGive(s_lock);
    if (slot < 0) {
        ESP_LOGW(TAG, "Too many event clients, refusing fd %d", fd);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Event client connected (fd %d)", fd);
    uint32_t ip = api_peer_ip(req);
    if (ip) voice_set_target(ip, 0);        // call audio goes to the newest client, port 4000 by default
    send_hello(fd);                          // queued: goes out after the handshake reply
    return ESP_OK;
}

static esp_err_t h_events(httpd_req_t *req)
{
#if !(CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT && CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT)
    if (req->method == HTTP_GET) {               // IDF 5.x: the handshake
        if (ws_check(req) != ESP_OK) return ESP_FAIL;
        return ws_open(req);
    }
#endif
    // Data frames from the client: we only expect pings/pongs (handled by the server) or text we ignore.
    httpd_ws_frame_t f = { 0 };
    esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
    if (r != ESP_OK) return r;
    if (f.len > 0 && f.len < 1024) {
        uint8_t *buf = calloc(1, f.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        f.payload = buf;
        r = httpd_ws_recv_frame(req, &f, f.len);
        // {"type": "audio", "port": 4000}: send call audio to this client on that UDP port
        cJSON *j = (r == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) ? cJSON_Parse((char *)buf) : NULL;
        const cJSON *t = cJSON_GetObjectItem(j, "type"), *p = cJSON_GetObjectItem(j, "port");
        if (cJSON_IsString(t) && strcmp(t->valuestring, "audio") == 0) {
            uint32_t ip = api_peer_ip(req);
            if (ip) voice_set_target(ip, cJSON_IsNumber(p) ? (uint16_t)p->valueint : 0);
        }
        cJSON_Delete(j);
        free(buf);
    }
    return r;
}

// Keepalive: a WebSocket ping every 20 s lets clients notice a dead link, and lets us drop dead fds.
static void ping_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        if (events_clients()) queue_send(NULL, -1, HTTPD_WS_TYPE_PING);
    }
}

void events_register(httpd_handle_t hd)
{
    s_hd = hd;
    s_lock = xSemaphoreCreateMutex();
    const httpd_uri_t u = { .uri = "/events", .method = HTTP_GET, .handler = h_events, .is_websocket = true,
                            .handle_ws_control_frames = false,
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT && CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
                            .ws_pre_handshake_cb = ws_check,
                            .ws_post_handshake_cb = ws_open,
#endif
    };
    httpd_register_uri_handler(hd, &u);
    xTaskCreatePinnedToCore(ping_task, "ws_ping", 3072, NULL, 3, NULL, 1);
}
