#include "modem.h"
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "modem";

#define MODEM_LINE_MAX 512
#define PDU_BUF_MAX 420

static SemaphoreHandle_t s_cmd_lock;      // one command at a time
static SemaphoreHandle_t s_done;          // given by the RX task when the command finishes
static SemaphoreHandle_t s_status_lock;
static modem_urc_cb_t s_urc_cbs[4];
static modem_status_t s_status = { .csq = 99 };

// State of the pending command, owned by the RX task while s_waiting is true.
static volatile bool s_waiting;
static volatile esp_err_t s_result;
static const char *s_prefix;
static char *s_resp;
static size_t s_resp_len, s_resp_used;

// Lines that are always unsolicited, even while a command is pending.
static const char *const URC_PREFIXES[] = {
    "RING", "+CLIP:", "+CMTI:", "+CMT:", "+CDS:", "+CDSI:", "NO CARRIER", "BUSY", "NO ANSWER",
    "VOICE CALL:", "MISSED_CALL:", "+CEREG:", "+CREG:", "+CGEV:", "SMS DONE", "PB DONE", "*ATREADY",
    "RDY", "+CPIN:", "+CLCC:", "+CRING:", "CALL READY",
};

static bool is_urc(const char *line)
{
    for (size_t i = 0; i < sizeof(URC_PREFIXES) / sizeof(URC_PREFIXES[0]); i++)
        if (strncmp(line, URC_PREFIXES[i], strlen(URC_PREFIXES[i])) == 0) return true;
    return false;
}

static void resp_append(const char *line)
{
    if (!s_resp || s_resp_len == 0) return;
    size_t n = strlen(line);
    if (s_resp_used + n + 2 > s_resp_len) return;
    if (s_resp_used) s_resp[s_resp_used++] = '\n';
    memcpy(s_resp + s_resp_used, line, n);
    s_resp_used += n;
    s_resp[s_resp_used] = 0;
}

static void finish(esp_err_t r)
{
    s_result = r;
    s_waiting = false;
    xSemaphoreGive(s_done);
}

static void handle_line(const char *line)
{
    ESP_LOGD(TAG, "<< %s", line);
    if (s_waiting) {
        if (strcmp(line, "OK") == 0 || strcmp(line, "> ") == 0) { finish(ESP_OK); return; }
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0 ||
            strncmp(line, "+CMS ERROR", 10) == 0) {
            resp_append(line);
            finish(ESP_FAIL);
            return;
        }
        // A line the command asked for wins over the URC list (e.g. AT+CEREG? -> +CEREG: 2,1).
        if (s_prefix ? strncmp(line, s_prefix, strlen(s_prefix)) == 0 : !is_urc(line)) {
            resp_append(line);
            return;
        }
    }
    ESP_LOGI(TAG, "URC: %s", line);
    for (int i = 0; i < 4; i++) if (s_urc_cbs[i]) s_urc_cbs[i](line);
}

static void rx_task(void *arg)
{
    static char line[MODEM_LINE_MAX];
    size_t n = 0;
    uint8_t buf[128];
    while (1) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (n < MODEM_LINE_MAX - 1) line[n++] = c;
                // The SMS prompt "> " has no line ending.
                if (!(n == 2 && line[0] == '>' && line[1] == ' ')) continue;
            }
            line[n] = 0;
            if (n) handle_line(line);
            n = 0;
        }
    }
}

// Sends data (plus CR when add_cr) and waits for the final line. Caller holds s_cmd_lock.
static esp_err_t exec_locked(const char *data, bool add_cr, const char *prefix, char *resp, size_t resp_len,
                             int timeout_ms)
{
    xSemaphoreTake(s_done, 0);
    if (resp && resp_len) resp[0] = 0;
    s_prefix = prefix;
    s_resp = resp;
    s_resp_len = resp_len;
    s_resp_used = 0;
    s_result = ESP_ERR_TIMEOUT;
    s_waiting = true;
    ESP_LOGD(TAG, ">> %s", data);
    uart_write_bytes(MODEM_UART, data, strlen(data));
    if (add_cr) uart_write_bytes(MODEM_UART, "\r", 1);
    esp_err_t r;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) r = s_result;
    else {
        s_waiting = false;
        r = ESP_ERR_TIMEOUT;
    }
    s_resp = NULL;
    return r;
}

esp_err_t modem_cmd(const char *cmd, const char *prefix, char *resp, size_t resp_len, int timeout_ms)
{
    if (!s_cmd_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(timeout_ms + 5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = exec_locked(cmd, true, prefix, resp, resp_len, timeout_ms);
    xSemaphoreGive(s_cmd_lock);
    return r;
}

esp_err_t modem_send_pdu(int tpdu_len, const char *hex, char *resp, size_t resp_len, int timeout_ms)
{
    if (!s_cmd_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(timeout_ms + 5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", tpdu_len);
    esp_err_t r = exec_locked(cmd, true, NULL, resp, resp_len, 5000);   // OK here means "> "
    if (r == ESP_OK) {
        static char buf[PDU_BUF_MAX];
        snprintf(buf, sizeof(buf), "%s\x1A", hex);
        r = exec_locked(buf, false, "+CMGS:", resp, resp_len, timeout_ms);
    } else if (r == ESP_ERR_TIMEOUT) {
        uart_write_bytes(MODEM_UART, "\x1B", 1);   // abort a half-open prompt
    }
    xSemaphoreGive(s_cmd_lock);
    return r;
}

void modem_add_urc_cb(modem_urc_cb_t cb)
{
    for (int i = 0; i < 4; i++) if (!s_urc_cbs[i]) { s_urc_cbs[i] = cb; return; }
    ESP_LOGE(TAG, "too many URC listeners");
}

void modem_get_status(modem_status_t *out)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
    out->powered = gpio_get_level(PIN_MODEM_PWR_EN);
    out->vbat = board_vbat();
}

#define STATUS_SET(stmt) do { xSemaphoreTake(s_status_lock, portMAX_DELAY); stmt; xSemaphoreGive(s_status_lock); } while (0)

void modem_init(void)
{
    uart_config_t uc = {
        .baud_rate = MODEM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART, PIN_MODEM_TX, PIN_MODEM_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_cmd_lock = xSemaphoreCreateMutex();
    s_status_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    // Core 1 with the network stack; core 0 is kept for the audio path later.
    xTaskCreatePinnedToCore(rx_task, "modem_rx", 4096, NULL, 10, NULL, 1);
}

esp_err_t modem_power_on(void)
{
    if (gpio_get_level(PIN_MODEM_PWR_EN) && modem_cmd("AT", NULL, NULL, 0, 500) == ESP_OK) return ESP_OK;
    float vbus = board_vbus();
    if (vbus < 4.6f) {
        ESP_LOGE(TAG, "VBUS is only %.2f V, use a better USB supply", vbus);
        return ESP_FAIL;
    }
    if (!gpio_get_level(PIN_MODEM_PWR_EN)) {
        // SIMCom: VBAT below 0.1 V for 2 s before powering up again
        for (int i = 0; i < 50 && board_vbat() > 0.1f; i++) vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(2000));
        gpio_set_level(PIN_MODEM_PWR_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    float vbat = board_vbat();
    if (vbat < 3.4f) {
        gpio_set_level(PIN_MODEM_PWR_EN, 0);
        ESP_LOGE(TAG, "VBAT only %.2f V, supply switched off again", vbat);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "VBAT %.2f V, pressing PWRKEY", vbat);
    gpio_set_level(PIN_MODEM_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_MODEM_PWRKEY, 0);
    // Don't trust STATUS: it can read high before the modem's 1.8 V I/O is up. Retry AT instead.
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (modem_cmd("AT", NULL, NULL, 0, 500) == ESP_OK) {
            ESP_LOGI(TAG, "Modem answered after %d s", i + 1);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No answer to AT after 30 s");
    return ESP_ERR_TIMEOUT;
}

esp_err_t modem_power_off(void)
{
    STATUS_SET(s_status.ready = false);
    if (gpio_get_level(PIN_MODEM_PWR_EN) && gpio_get_level(PIN_MODEM_STATUS)) {
        modem_cmd("AT+CPOF", NULL, NULL, 0, 3000);
        for (int t = 0; gpio_get_level(PIN_MODEM_STATUS) && t < 150; t++) vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(PIN_MODEM_STATUS)) ESP_LOGW(TAG, "STATUS still high after 15 s, cutting power");
    }
    gpio_set_level(PIN_MODEM_PWR_EN, 0);
    board_led(PIN_LED_GREEN, false);
    ESP_LOGI(TAG, "Modem off");
    return ESP_OK;
}

static void copy_after_colon(const char *src, char *dst, size_t len)
{
    const char *p = strchr(src, ':');
    p = p ? p + 1 : src;
    while (*p == ' ') p++;
    strlcpy(dst, p, len);
}

static void configure(void)
{
    char r[128];
    modem_cmd("ATE0", NULL, NULL, 0, 1000);          // no echo
    modem_cmd("AT+CMEE=2", NULL, NULL, 0, 1000);     // readable errors
    modem_cmd("AT+CNMP=38", NULL, NULL, 0, 3000);    // LTE only: no 2 A 2G bursts
    modem_cmd("AT+CLIP=1", NULL, NULL, 0, 1000);     // caller ID with RING
    modem_cmd("AT+CMGF=0", NULL, NULL, 0, 1000);     // SMS in PDU mode
    modem_cmd("AT+CEREG=2", NULL, NULL, 0, 1000);    // registration changes as URCs
    modem_cmd("AT+COPS=3,0", NULL, NULL, 0, 1000);   // operator as a long name
    modem_cmd("AT+CPMS=\"ME\",\"ME\",\"ME\"", NULL, NULL, 0, 5000);   // SMS in modem memory, not the SIM
    modem_cmd("AT+CNMI=2,1,0,0,0", NULL, NULL, 0, 1000);  // new SMS -> "+CMTI: <mem>,<index>"

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    modem_status_t st = s_status;
    xSemaphoreGive(s_status_lock);
    if (modem_cmd("AT+CGSN", NULL, r, sizeof(r), 2000) == ESP_OK) strlcpy(st.imei, r, sizeof(st.imei));
    if (modem_cmd("AT+CGMR", "+CGMR:", r, sizeof(r), 2000) == ESP_OK) copy_after_colon(r, st.revision, sizeof(st.revision));
    if (modem_cmd("AT+CICCID", "+ICCID:", r, sizeof(r), 2000) == ESP_OK) copy_after_colon(r, st.iccid, sizeof(st.iccid));
    st.ready = true;
    st.at_errors = 0;
    st.generation++;
    STATUS_SET(s_status = st);
}

static void poll(void)
{
    char r[128];
    modem_status_t st;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    st = s_status;
    xSemaphoreGive(s_status_lock);

    esp_err_t e = modem_cmd("AT+CSQ", "+CSQ:", r, sizeof(r), 2000);
    if (e == ESP_ERR_TIMEOUT) st.at_errors++;
    else st.at_errors = 0;
    if (e == ESP_OK) {
        st.csq = atoi(strchr(r, ':') + 1);
        st.rssi_dbm = (st.csq >= 0 && st.csq <= 31) ? -113 + 2 * st.csq : 0;
    }
    if (modem_cmd("AT+CPIN?", "+CPIN:", r, sizeof(r), 5000) == ESP_OK) copy_after_colon(r, st.sim, sizeof(st.sim));
    else strlcpy(st.sim, "ERROR", sizeof(st.sim));
    if (modem_cmd("AT+CEREG?", "+CEREG:", r, sizeof(r), 2000) == ESP_OK) {
        const char *c = strchr(r, ',');    // +CEREG: <n>,<stat>[,...]
        st.reg = c ? atoi(c + 1) : 0;
    }
    if (modem_cmd("AT+COPS?", "+COPS:", r, sizeof(r), 5000) == ESP_OK) {
        const char *q = strchr(r, '"');
        st.operator_name[0] = 0;
        if (q) {
            strlcpy(st.operator_name, q + 1, sizeof(st.operator_name));
            char *end = strchr(st.operator_name, '"');
            if (end) *end = 0;
        }
    }
    board_led(PIN_LED_GREEN, st.reg == 1 || st.reg == 5);
    STATUS_SET(s_status = st);
}

static void manager_task(void *arg)
{
    while (1) {
        if (modem_power_on() == ESP_OK) {
            configure();
            while (1) {
                poll();
                modem_status_t st;
                modem_get_status(&st);
                if (st.at_errors >= 3) {
                    ESP_LOGE(TAG, "Modem stopped answering, power-cycling it");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(30000));
            }
        }
        modem_power_off();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void modem_start(void)
{
    xTaskCreatePinnedToCore(manager_task, "modem_mgr", 6144, NULL, 5, NULL, 1);
}
