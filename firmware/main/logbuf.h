// Copy of the ESP_LOG output in a RAM ring buffer, so the log can be read over HTTP (GET /logs)
// when no USB console is attached. Plain printf output (console commands) is not captured.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Hooks esp_log; call first thing in app_main. The UART output is unchanged.
void logbuf_init(void);

// Copies the log written since position `since` (0 = everything still in the buffer) into out,
// at most max bytes. *next is the position to pass next time. Returns the byte count.
// Positions count every byte ever written, so a reader that falls behind skips to the oldest
// byte still kept.
size_t logbuf_read(uint32_t since, char *out, size_t max, uint32_t *next);

size_t logbuf_capacity(void);

// Random per boot: positions from a previous boot mean nothing, so a reader seeing a new id
// starts again from 0.
uint32_t logbuf_boot_id(void);
