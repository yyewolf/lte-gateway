#include "logbuf.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define CAP_PSRAM (64 * 1024)
#define CAP_INTERNAL (8 * 1024)
#define LOGBUF_LINE_MAX 384

static char *s_buf;
static size_t s_cap;
static uint32_t s_head;               // bytes ever written
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_uart;         // the previous handler (prints to the console)
static char s_line[LOGBUF_LINE_MAX];         // only touched with s_lock held
static uint32_t s_boot;

// Appends s to the ring without the ANSI colour codes. s_lock held.
static void append(const char *s, int n)
{
    static bool esc;                  // inside an ESC [ ... <letter> sequence
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (esc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = false;
            continue;
        }
        if (c == '\x1b') {
            esc = true;
            continue;
        }
        s_buf[s_head % s_cap] = c;
        s_head++;
    }
}

static int hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_uart(fmt, ap);
    // Skip ISRs and critical sections (can't take a mutex there). Lines logged while another
    // task holds the lock for longer than 20 ms are dropped from the copy, not from the UART.
    if (xPortCanYield() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        if (n > 0) {
            if (n >= (int)sizeof(s_line)) {
                n = sizeof(s_line) - 1;
                s_line[n - 1] = '\n';     // truncated: keep the line break
            }
            append(s_line, n);
        }
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return r;
}

void logbuf_init(void)
{
    if (s_buf) return;
    s_buf = heap_caps_malloc(CAP_PSRAM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cap = CAP_PSRAM;
    if (!s_buf) {
        s_buf = heap_caps_malloc(CAP_INTERNAL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_cap = CAP_INTERNAL;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_buf || !s_lock) {
        free(s_buf);
        s_buf = NULL;
        return;
    }
    s_boot = esp_random();
    s_uart = esp_log_set_vprintf(hook);
}

size_t logbuf_read(uint32_t since, char *out, size_t max, uint32_t *next)
{
    *next = 0;
    if (!s_buf) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t head = s_head;
    uint32_t oldest = head > s_cap ? head - s_cap : 0;
    if (since < oldest || since > head) since = oldest;
    size_t n = head - since;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = s_buf[(since + i) % s_cap];
    *next = since + n;
    xSemaphoreGive(s_lock);
    return n;
}

size_t logbuf_capacity(void) { return s_buf ? s_cap : 0; }

uint32_t logbuf_boot_id(void) { return s_boot; }
