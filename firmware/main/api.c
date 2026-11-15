#include "api.h"
#include "app.h"
#include "net.h"
#include "modem.h"
#include "board.h"
#include "settings.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "sms.h"
#include "sms_store.h"
#include "calls.h"
#include "events.h"
#include "voice.h"
#include "peers.h"
#include "ota.h"
#include "ui.h"
#include "microlink.h"
#include <stdlib.h>

static const char *TAG = "api";
static httpd_handle_t s_httpd;

// IPv4 address of a sockaddr (plain or IPv4-mapped IPv6), host byte order. 0 if not IPv4.
static uint32_t v4_of(const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET) return ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr);
#if CONFIG_LWIP_IPV6
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;
        const uint32_t *w = (const uint32_t *)&s6->sin6_addr;
        if (w[0] == 0 && w[1] == 0 && w[2] == htonl(0xffff)) return ntohl(w[3]);
    }
#endif
    return 0;
}

// The request must have come in on our tailnet IP, from a tailnet address (100.64.0.0/10).
// WireGuard only hands us packets from authenticated peers, and replies to 100.x go back
// through the tunnel, so a LAN host can't complete a TCP handshake with a forged source.
bool api_from_tailnet(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = { 0 }, peer = { 0 };
    socklen_t ll = sizeof(local), pl = sizeof(peer);
    if (getsockname(fd, (struct sockaddr *)&local, &ll) != 0) return false;
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0) return false;
    uint32_t vpn = net_tailnet_ip();
    uint32_t l = v4_of(&local), p = v4_of(&peer);
    return vpn != 0 && l == vpn && (p & 0xFFC00000u) == 0x64400000u;
}

// The request came in on our WiFi (LAN) IP. Only the peers page and its endpoints accept this:
// it is how to pin a peer when no useful peer can reach the gateway over the tailnet.
static bool from_lan(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = { 0 };
    socklen_t ll = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &ll) != 0) return false;
    uint32_t lan = net_lan_ip();
    return lan != 0 && v4_of(&local) == lan;
}

uint32_t api_peer_ip(httpd_req_t *req)
{
    struct sockaddr_storage peer = { 0 };
    socklen_t pl = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &pl) != 0) return 0;
    return v4_of(&peer);
}

static bool token_ok(httpd_req_t *req)
{
    char token[80];
    if (!settings_get("api_token", token, sizeof(token))) return true;
    char hdr[100];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    return strncmp(hdr, "Bearer ", 7) == 0 && strcmp(hdr + 7, token) == 0;
}

esp_err_t api_guard(httpd_req_t *req)
{
    if (!api_from_tailnet(req)) {
        ESP_LOGW(TAG, "Refused %s from outside the tailnet", req->uri);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "tailnet only");
        return ESP_FAIL;
    }
    if (!token_ok(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Like api_guard, but the LAN is accepted too (peers page and pinning only). Same token rule.
static esp_err_t lan_guard(httpd_req_t *req)
{
    if (!api_from_tailnet(req) && !from_lan(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "tailnet or LAN only");
        return ESP_FAIL;
    }
    if (!token_ok(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return r;
}

static const char *reg_name(int reg)
{
    switch (reg) {
    case 0: return "not_searching";
    case 1: return "home";
    case 2: return "searching";
    case 3: return "denied";
    case 5: return "roaming";
    default: return "unknown";
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    net_status_t n;
    modem_status_t m;
    net_get_status(&n);
    modem_get_status(&m);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware", GW_VERSION);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "vbus_v", board_vbus());

    cJSON *w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(w, "connected", n.wifi_up);
    cJSON_AddStringToObject(w, "ssid", n.ssid);
    cJSON_AddNumberToObject(w, "rssi_dbm", n.rssi);
    cJSON_AddStringToObject(w, "ip", n.lan_ip);

    cJSON *t = cJSON_AddObjectToObject(root, "tailnet");
    cJSON_AddStringToObject(t, "state", n.tailnet_state);
    cJSON_AddStringToObject(t, "ip", n.tailnet_ip);
    cJSON_AddNumberToObject(t, "peers", n.peers);

    cJSON *mo = cJSON_AddObjectToObject(root, "modem");
    cJSON_AddBoolToObject(mo, "powered", m.powered);
    cJSON_AddBoolToObject(mo, "ready", m.ready);
    cJSON_AddNumberToObject(mo, "vbat_v", m.vbat);
    cJSON_AddStringToObject(mo, "sim", m.sim);
    cJSON_AddStringToObject(mo, "registration", reg_name(m.reg));
    cJSON_AddStringToObject(mo, "operator", m.operator_name);
    if (m.rssi_dbm) cJSON_AddNumberToObject(mo, "rssi_dbm", m.rssi_dbm);
    else cJSON_AddNullToObject(mo, "rssi_dbm");
    cJSON_AddStringToObject(mo, "imei", m.imei);
    cJSON_AddStringToObject(mo, "iccid", m.iccid);
    cJSON_AddStringToObject(mo, "revision", m.revision);
    cJSON_AddNumberToObject(root, "sms_inbox", sms_inbox_count());
    cJSON_AddItemToObject(root, "call", calls_state_json());
    cJSON_AddNumberToObject(root, "event_clients", events_clients());
    cJSON_AddItemToObject(root, "voice", voice_stats_json());
    return send_json(req, root);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return send_json(req, r);
}

// Body of at most max bytes, NUL-terminated, malloc'd. NULL on error (response already sent).
static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        send_err(req, "413 Payload Too Large", "body missing or too large");
        return NULL;
    }
    char *b = malloc(req->content_len + 1);
    size_t got = 0;
    while (b && got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    if (!b || got != req->content_len) {
        free(b);
        send_err(req, "400 Bad Request", "could not read body");
        return NULL;
    }
    b[got] = 0;
    return b;
}

// Trailing number of the URI, e.g. /sms/inbox/12 -> 12. 0 if none.
static uint32_t uri_id(httpd_req_t *req)
{
    const char *s = strrchr(req->uri, '/');
    return s ? strtoul(s + 1, NULL, 10) : 0;
}

static const char *out_state(uint8_t s)
{
    return s == SMS_OUT_SENT ? "sent" : s == SMS_OUT_FAILED ? "failed" : "queued";
}

static cJSON *in_json(const sms_in_t *m)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", m->id);
    cJSON_AddStringToObject(o, "from", m->from);
    cJSON_AddStringToObject(o, "timestamp", m->timestamp);
    cJSON_AddStringToObject(o, "text", m->text ? m->text : "");
    cJSON_AddNumberToObject(o, "parts", m->parts);
    cJSON_AddBoolToObject(o, "complete", m->complete);
    return o;
}

// POST /sms {"to": "+336...", "text": "..."}
static esp_err_t h_sms_send(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    char *body = read_body(req, 8192);
    if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *to = cJSON_GetObjectItem(j, "to"), *text = cJSON_GetObjectItem(j, "text");
    if (!cJSON_IsString(to) || !cJSON_IsString(text)) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "expected JSON {\"to\": \"+33...\", \"text\": \"...\"}");
    }
    uint32_t id;
    int parts;
    bool ucs2;
    const char *why;
    esp_err_t e = sms_send(to->valuestring, text->valuestring, &id, &parts, &ucs2, &why);
    cJSON_Delete(j);
    if (e != ESP_OK) return send_err(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", why);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddNumberToObject(r, "parts", parts);
    cJSON_AddStringToObject(r, "encoding", ucs2 ? "ucs2" : "gsm7");
    cJSON_AddStringToObject(r, "state", "queued");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, r);
}

// GET /sms/outbox/<id>
static esp_err_t h_outbox_get(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    sms_out_t m;
    if (sms_outbox_get(uri_id(req), &m) != ESP_OK) return send_err(req, "404 Not Found", "no such message");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", m.id);
    cJSON_AddStringToObject(o, "to", m.to);
    cJSON_AddStringToObject(o, "state", out_state(m.state));
    cJSON_AddNumberToObject(o, "parts", m.parts);
    cJSON_AddNumberToObject(o, "sent_parts", m.sent_parts);
    cJSON_AddNumberToObject(o, "attempts", m.attempts);
    if (m.error[0]) cJSON_AddStringToObject(o, "error", m.error);
    sms_out_free(&m);
    return send_json(req, o);
}

// GET /sms/inbox?after=<id>&limit=<n>   (oldest first)
static esp_err_t h_inbox_list(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    uint32_t after = 0;
    int limit = 20;
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "after", v, sizeof(v)) == ESP_OK) after = strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) limit = atoi(v);
    }
    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;
    uint32_t ids[50];
    int n = sms_inbox_list(after, ids, limit);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "messages");
    for (int i = 0; i < n; i++) {
        sms_in_t m;
        if (sms_inbox_get(ids[i], &m) != ESP_OK) continue;
        cJSON_AddItemToArray(arr, in_json(&m));
        sms_in_free(&m);
    }
    cJSON_AddNumberToObject(r, "total", sms_inbox_count());
    return send_json(req, r);
}

// GET /sms/inbox/<id>
static esp_err_t h_inbox_get(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    if (strncmp(req->uri, "/sms/inbox?", 11) == 0) return h_inbox_list(req);
    sms_in_t m;
    if (sms_inbox_get(uri_id(req), &m) != ESP_OK) return send_err(req, "404 Not Found", "no such message");
    cJSON *o = in_json(&m);
    sms_in_free(&m);
    return send_json(req, o);
}

// DELETE /sms/inbox/<id>: the server has stored the message, drop it from flash
static esp_err_t h_inbox_delete(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    if (sms_inbox_delete(uri_id(req)) != ESP_OK) return send_err(req, "404 Not Found", "no such message");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ---------- calls ----------
static esp_err_t h_call_get(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    return send_json(req, calls_state_json());
}

static esp_err_t call_result(httpd_req_t *req, esp_err_t e, const char *why)
{
    if (e == ESP_OK) return send_json(req, calls_state_json());
    return send_err(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : e == ESP_ERR_INVALID_STATE ? "409 Conflict"
                                                                                                 : "502 Bad Gateway", why);
}

// POST /call/dial {"number": "+33..."}
static esp_err_t h_call_dial(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    char *body = read_body(req, 512);
    if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *n = cJSON_GetObjectItem(j, "number");
    const char *why = NULL;
    esp_err_t e = cJSON_IsString(n) ? calls_dial(n->valuestring, &why) : ESP_ERR_INVALID_ARG;
    if (!cJSON_IsString(n)) why = "expected JSON {\"number\": \"+33...\"}";
    cJSON_Delete(j);
    return call_result(req, e, why);
}

static esp_err_t h_call_answer(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    const char *why = NULL;
    esp_err_t e = calls_answer(&why);
    return call_result(req, e, why);
}

static esp_err_t h_call_hangup(httpd_req_t *req)
{
    if (api_guard(req) != ESP_OK) return ESP_OK;
    const char *why = NULL;
    esp_err_t e = calls_hangup(&why);
    return call_result(req, e, why);
}

// ---------- peers ----------
static esp_err_t h_peers_get(httpd_req_t *req)
{
    if (lan_guard(req) != ESP_OK) return ESP_OK;
    return send_json(req, peers_json());
}

// POST /peers/pin and /peers/unpin {"ip": "100.x.y.z"}
static esp_err_t pin_common(httpd_req_t *req, bool pin)
{
    if (lan_guard(req) != ESP_OK) return ESP_OK;
    char *body = read_body(req, 256);
    if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *ip = cJSON_GetObjectItem(j, "ip");
    uint32_t v = cJSON_IsString(ip) ? microlink_parse_ip(ip->valuestring) : 0;
    cJSON_Delete(j);
    if (!v) return send_err(req, "400 Bad Request", "expected JSON {\"ip\": \"100.x.y.z\"}");
    const char *why = NULL;
    esp_err_t e = pin ? peers_pin(v, &why) : peers_unpin(v, &why);
    if (e != ESP_OK)
        return send_err(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : e == ESP_ERR_NOT_FOUND ? "404 Not Found"
                             : e == ESP_ERR_NO_MEM ? "409 Conflict" : "500 Internal Server Error", why);
    return send_json(req, peers_json());
}

static esp_err_t h_peers_pin(httpd_req_t *req) { return pin_common(req, true); }
static esp_err_t h_peers_unpin(httpd_req_t *req) { return pin_common(req, false); }

// GET /ui: admin page, tailnet or LAN; the page's own requests carry the token. Over the
// LAN only its peers part works (status and firmware stay tailnet only).
static esp_err_t h_ui(httpd_req_t *req)
{
    if (!api_from_tailnet(req) && !from_lan(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "tailnet or LAN only");
        return ESP_OK;
    }
    return ui_send_page(req);
}

static esp_err_t h_root(httpd_req_t *req)
{
    // A browser gets the admin page
    char accept[96];
    if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) != ESP_ERR_NOT_FOUND &&
        strstr(accept, "text/html") && (api_from_tailnet(req) || from_lan(req))) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ui");
        return httpd_resp_send(req, NULL, 0);
    }
    if (api_guard(req) != ESP_OK) return ESP_OK;
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "LTE gateway " GW_VERSION ". Endpoints:\n"
                                   "  GET /status\n"
                                   "  POST /sms {\"to\":\"+33...\",\"text\":\"...\"}\n"
                                   "  GET /sms/outbox/<id>\n"
                                   "  GET /sms/inbox[?after=<id>&limit=<n>]\n"
                                   "  GET /sms/inbox/<id>\n"
                                   "  DELETE /sms/inbox/<id>\n"
                                   "  GET /call\n"
                                   "  POST /call/dial {\"number\":\"+33...\"}\n"
                                   "  POST /call/answer\n"
                                   "  POST /call/hangup\n"
                                   "  WebSocket /events\n"
                                   "  GET /peers\n"
                                   "  POST /peers/pin {\"ip\":\"100.x.y.z\"}\n"
                                   "  POST /peers/unpin {\"ip\":\"100.x.y.z\"}\n"
                                   "  GET /ota\n"
                                   "  POST /ota  (body: build/lte_gateway.bin)\n"
                                   "  POST /ota/rollback\n"
                                   "  GET /ui  (admin page, in a browser)\n");
}

void api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;
    cfg.core_id = 1;              // core 0 is kept for the audio path later
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 32;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/sms", .method = HTTP_POST, .handler = h_sms_send },
        { .uri = "/sms/outbox/*", .method = HTTP_GET, .handler = h_outbox_get },
        { .uri = "/sms/inbox", .method = HTTP_GET, .handler = h_inbox_list },
        { .uri = "/sms/inbox/*", .method = HTTP_GET, .handler = h_inbox_get },
        { .uri = "/sms/inbox/*", .method = HTTP_DELETE, .handler = h_inbox_delete },
        { .uri = "/call", .method = HTTP_GET, .handler = h_call_get },
        { .uri = "/call/dial", .method = HTTP_POST, .handler = h_call_dial },
        { .uri = "/call/answer", .method = HTTP_POST, .handler = h_call_answer },
        { .uri = "/call/hangup", .method = HTTP_POST, .handler = h_call_hangup },
        { .uri = "/peers", .method = HTTP_GET, .handler = h_peers_get },
        { .uri = "/peers/pin", .method = HTTP_POST, .handler = h_peers_pin },
        { .uri = "/peers/unpin", .method = HTTP_POST, .handler = h_peers_unpin },
        { .uri = "/ui", .method = HTTP_GET, .handler = h_ui },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    events_register(s_httpd);
    ota_register(s_httpd);
    ESP_LOGI(TAG, "API on port 80 (tailnet only)");
}
