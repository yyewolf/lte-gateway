// Admin page (peers + firmware). The caller checks that the request came through the tailnet.
#pragma once
#include "esp_http_server.h"
esp_err_t ui_send_page(httpd_req_t *req);
