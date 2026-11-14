#include "setup.h"
#include "app.h"
#include "settings.h"
#include "board.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

static const char *TAG = "setup";

#define SETUP_TIMEOUT_US (15LL * 60 * 1000000)   // back to normal mode if nothing is saved
#define MAX_APS 20

static char s_ap_ssid[33];
static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;
static volatile int64_t s_last_activity;

// ---------- captive DNS: answer every A query with 192.168.4.1 ----------
static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        vTaskDelete(NULL);
    }
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;   // queries with one question only
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;                        // skip the name
        if (p + 5 > n) continue;
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int end = p + 5;
        buf[2] = 0x81; buf[3] = 0x80;                                   // response, recursion available
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;                      // answers
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + end, ans, sizeof(ans));
            end += sizeof(ans);
        }
        sendto(fd, buf, end, 0, (struct sockaddr *)&from, fl);
    }
}

// ---------- helpers ----------
static void html_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// Finds `key` in an application/x-www-form-urlencoded body and decodes it into out.
static bool form_get(const char *body, const char *key, char *out, size_t len)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) != 0 || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t o = 0;
        while (*v && *v != '&' && o + 1 < len) {
            if (*v == '+') { out[o++] = ' '; v++; }
            else if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                out[o++] = (char)(hexval(v[1]) << 4 | hexval(v[2]));
                v += 3;
            } else out[o++] = *v++;
        }
        out[o] = 0;
        return true;
    }
    out[0] = 0;
    return false;
}

// ---------- pages ----------
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>LTE gateway setup</title><style>"
    ":root{--bg:#f3f4f6;--card:#fff;--fg:#111827;--muted:#6b7280;--line:#e5e7eb;--acc:#2563eb;--acc2:#1d4ed8}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0b1120;--card:#111827;--fg:#f3f4f6;--muted:#9ca3af;--line:#1f2937;--acc:#3b82f6;--acc2:#60a5fa}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
    "font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:30rem;margin:0 auto;padding:1.25rem 1rem 3rem}"
    "header{display:flex;align-items:center;gap:.75rem;margin:.5rem 0 1.25rem}"
    ".logo{width:2.5rem;height:2.5rem;border-radius:.75rem;background:var(--acc);color:#fff;display:grid;place-items:center;font-weight:700}"
    "h1{font-size:1.25rem;margin:0}header p{margin:0;color:var(--muted);font-size:.875rem}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:1rem;padding:1rem 1.1rem;margin-bottom:1rem}"
    "h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 .75rem}"
    "label{display:block;font-weight:600;font-size:.95rem;margin:.9rem 0 .3rem}label:first-of-type{margin-top:0}"
    "small{display:block;color:var(--muted);font-weight:400;font-size:.8rem;margin-top:.15rem}"
    "input{width:100%;padding:.65rem .75rem;font-size:1rem;border:1px solid var(--line);border-radius:.6rem;"
    "background:var(--bg);color:var(--fg)}input:focus{outline:2px solid var(--acc);border-color:transparent}"
    ".nets{list-style:none;margin:0 0 .75rem;padding:0;border:1px solid var(--line);border-radius:.6rem;max-height:14rem;overflow:auto}"
    ".nets li{display:flex;justify-content:space-between;align-items:center;padding:.6rem .75rem;cursor:pointer;border-top:1px solid var(--line)}"
    ".nets li:first-child{border-top:0}.nets li:hover,.nets li.sel{background:var(--bg)}.nets li.sel{box-shadow:inset 3px 0 var(--acc)}"
    ".nets .m{color:var(--muted);font-size:.8rem;white-space:nowrap}.nets .e{color:var(--muted);cursor:default}"
    ".row{display:flex;justify-content:space-between;align-items:center;margin-bottom:.5rem}"
    ".link{background:none;border:0;color:var(--acc);font-size:.875rem;cursor:pointer;padding:0}"
    "button.go{width:100%;padding:.85rem;font-size:1rem;font-weight:600;border:0;border-radius:.7rem;background:var(--acc);color:#fff;cursor:pointer}"
    "button.go:hover{background:var(--acc2)}.note{color:var(--muted);font-size:.85rem;text-align:center;margin-top:1rem}"
    "</style></head><body><main><header><div class=logo>GW</div><div><h1>LTE gateway setup</h1>"
    "<p>SMS + voice gateway &middot; firmware " GW_VERSION "</p></div></header>";

static void field(httpd_req_t *req, const char *key, const char *label, const char *hint, bool secret,
                  const char *extra)
{
    char cur[160], esc[400], line[1000];
    bool set = settings_get(key, cur, sizeof(cur));
    html_escape(esc, sizeof(esc), secret ? "" : cur);
    snprintf(line, sizeof(line),
             "<label for=%s>%s%s%s%s</label><input id=%s name=%s type=%s value=\"%s\" placeholder=\"%s\" %s "
             "autocomplete=off autocapitalize=off spellcheck=false>",
             key, label, hint[0] ? "<small>" : "", hint, hint[0] ? "</small>" : "", key, key,
             secret ? "password" : "text", esc, secret ? (set ? "Saved. Leave empty to keep it" : "") : "",
             extra ? extra : "");
    httpd_resp_sendstr_chunk(req, line);
}

static const char *bars(int rssi) { return rssi > -55 ? "&#9679;&#9679;&#9679;&#9679;" : rssi > -67 ? "&#9679;&#9679;&#9679;&#9675;" : rssi > -78 ? "&#9679;&#9679;&#9675;&#9675;" : "&#9679;&#9675;&#9675;&#9675;"; }

// Scan, drop hidden networks and duplicates (keeps the strongest), strongest first.
static void do_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { s_ap_count = 0; return; }
    uint16_t n = MAX_APS;
    wifi_ap_record_t raw[MAX_APS];
    if (esp_wifi_scan_get_ap_records(&n, raw) != ESP_OK) n = 0;
    s_ap_count = 0;
    for (int i = 0; i < n; i++) {
        if (!raw[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < s_ap_count && !dup; k++) dup = strcmp((char *)s_aps[k].ssid, (char *)raw[i].ssid) == 0;
        if (!dup) s_aps[s_ap_count++] = raw[i];
    }
    ESP_LOGI(TAG, "WiFi scan: %d network(s)", s_ap_count);
}

static void send_net_list(httpd_req_t *req)
{
    if (!s_ap_count) {
        httpd_resp_sendstr_chunk(req, "<li class=e>No networks found. Tap Rescan, or type the name below.</li>");
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        char esc[100], li[400];
        html_escape(esc, sizeof(esc), (const char *)s_aps[i].ssid);
        snprintf(li, sizeof(li), "<li data-s=\"%s\"><span>%s</span><span class=m>%s%s</span></li>", esc, esc,
                 s_aps[i].authmode == WIFI_AUTH_OPEN ? "open &middot; " : "&#128274; ", bars(s_aps[i].rssi));
        httpd_resp_sendstr_chunk(req, li);
    }
}

static const char PAGE_JS[] =
    "<script>"
    "var L=document.getElementById('nets'),S=document.getElementById('wifi_ssid'),P=document.getElementById('wifi_pass');"
    "function mark(){for(var i=0;i<L.children.length;i++){var e=L.children[i];e.className=(e.dataset.s&&e.dataset.s===S.value)?'sel':(e.dataset.s?'':'e')}}"
    "L.onclick=function(ev){var li=ev.target.closest('li');if(li&&li.dataset.s){S.value=li.dataset.s;mark();P.focus()}};"
    "S.oninput=mark;mark();"
    "document.getElementById('rescan').onclick=function(){var b=this;b.textContent='Scanning...';"
    "fetch('/scan').then(function(r){return r.text()}).then(function(h){L.innerHTML=h;mark();b.textContent='Rescan'})"
    ".catch(function(){b.textContent='Rescan'})};"
    "</script>";

static esp_err_t h_scan(httpd_req_t *req)
{
    s_last_activity = esp_timer_get_time();
    do_scan();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    send_net_list(req);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_form(httpd_req_t *req)
{
    s_last_activity = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<form method=post action=/save><section class=card><h2>WiFi</h2>"
                                  "<div class=row><span>Networks nearby</span><button type=button class=link id=rescan>Rescan</button></div>"
                                  "<ul class=nets id=nets>");
    send_net_list(req);
    httpd_resp_sendstr_chunk(req, "</ul>");
    field(req, "wifi_ssid", "Network name", "", false, "required");
    field(req, "wifi_pass", "Password", "", true, NULL);
    httpd_resp_sendstr_chunk(req, "</section><section class=card><h2>Tailscale</h2>");
    field(req, "ts_authkey", "Auth key", "tskey-auth-&hellip; from the admin console. Only needed for the first login.", true, NULL);
    field(req, "hostname", "Device name", "Name on your tailnet. Default: lte-gateway", false, NULL);
    field(req, "priority_peer", "Server tailnet IP", "Optional, 100.x.y.z. Always keeps a tunnel to it.", false, "inputmode=decimal");
    httpd_resp_sendstr_chunk(req, "</section><section class=card><h2>API</h2>");
    field(req, "api_token", "API token", "Optional. When set, requests need Authorization: Bearer &lt;token&gt;", true, NULL);
    httpd_resp_sendstr_chunk(req, "</section><button type=submit class=go>Save and restart</button>"
                                  "<p class=note>This page only exists in setup mode.</p></form>");
    httpd_resp_sendstr_chunk(req, PAGE_JS);
    httpd_resp_sendstr_chunk(req, "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_save(httpd_req_t *req)
{
    s_last_activity = esp_timer_get_time();
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }
    char *body = calloc(1, req->content_len + 1);
    int got = 0;
    while (body && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) break;
        got += r;
    }
    if (!body || got != req->content_len) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }
    char val[160];
    for (size_t i = 0; i < g_settings_count; i++) {
        const setting_def_t *s = &g_settings[i];
        if (!form_get(body, s->key, val, sizeof(val))) continue;
        if (s->secret && !val[0]) continue;     // empty password field = keep the saved one
        settings_set(s->key, val);
    }
    free(body);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<section class=card><h2>Saved</h2><p>The gateway restarts and joins your WiFi "
                                  "and tailnet. This setup network disappears; you can close this page.</p>"
                                  "<p>The red LED turns off once it is on the tailnet.</p></section></main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gw_restart();
    return ESP_OK;
}

static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void setup_mode_run(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "LTE-Gateway-setup-%02X%02X", mac[4], mac[5]);

    // Scan first so the page can offer the networks around.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    do_scan();

    httpd_handle_t httpd;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&httpd, &cfg));
    const httpd_uri_t form = { .uri = "/", .method = HTTP_GET, .handler = h_form };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    const httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = h_scan };
    httpd_register_uri_handler(httpd, &scan);
    httpd_register_uri_handler(httpd, &form);
    httpd_register_uri_handler(httpd, &save);
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, h_redirect);   // captive portal checks
    xTaskCreatePinnedToCore(dns_task, "dns", 4096, NULL, 4, NULL, 1);

    ESP_LOGW(TAG, "Setup mode: join WiFi \"%s\" and open http://192.168.4.1/", s_ap_ssid);

    char ssid[33];
    bool configured = settings_get("wifi_ssid", ssid, sizeof(ssid));
    s_last_activity = esp_timer_get_time();
    for (int i = 0;; i++) {                 // blink red while in setup mode
        board_led(PIN_LED_RED, i & 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        if (configured && esp_timer_get_time() - s_last_activity > SETUP_TIMEOUT_US) {
            ESP_LOGW(TAG, "Nothing saved for 15 minutes, back to normal mode");
            gw_restart();
        }
    }
}
