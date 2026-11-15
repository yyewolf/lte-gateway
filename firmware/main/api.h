// HTTP API, reachable only through the tailnet.
#pragma once
#include "esp_http_server.h"
void api_start(void);
// ESP_OK when the request came through the tailnet (and has the token, if one is set);
// otherwise sends 403/401 and returns ESP_FAIL.
esp_err_t api_guard(httpd_req_t *req);
// true when the request came in on our tailnet IP from a tailnet address (no token check)
bool api_from_tailnet(httpd_req_t *req);
uint32_t api_peer_ip(httpd_req_t *req);   // IPv4 of the client, host byte order, 0 if unknown
