#pragma once
#define GW_VERSION "0.3.0"
// Clean modem shutdown, then restart (an ESP32 reset cuts the modem supply).
void gw_restart(void);
// Restart into setup mode (setup WiFi + page).
void gw_restart_into_setup(void);
