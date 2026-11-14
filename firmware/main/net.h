// WiFi station + microlink (Tailscale). Normal mode only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool wifi_up;
    char ssid[33];
    int rssi;
    char lan_ip[16];
    const char *tailnet_state;    // IDLE, CONNECTING, CONNECTED, ...
    bool tailnet_up;
    char tailnet_ip[16];
    int peers;
} net_status_t;

void net_start(void);
void net_get_status(net_status_t *out);
uint32_t net_tailnet_ip(void);     // host byte order, 0 when not connected
void net_print_peers(void);

typedef struct microlink_s microlink_t;
microlink_t *net_microlink(void);   // NULL before net_start
