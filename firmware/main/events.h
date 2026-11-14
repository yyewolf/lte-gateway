// Push events to tailnet clients over a WebSocket: ws://<gateway>/events
#pragma once
#include "esp_http_server.h"
#include "cJSON.h"

void events_register(httpd_handle_t hd);     // called by api_start
// Sends {"type": type, "ts": uptime_ms, ...fields} to every connected client. Takes ownership of `fields`
// (may be NULL). Safe from any task.
void events_publish(const char *type, cJSON *fields);
int events_clients(void);
