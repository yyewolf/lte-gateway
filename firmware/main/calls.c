#include "calls.h"
#include "modem.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "calls";

#define URC_LEN 128
static QueueHandle_t s_urcs;               // URC lines copied out of the modem RX task
static SemaphoreHandle_t s_lock;
static call_info_t s_call;
static calls_cb_t s_cb;
static int64_t s_last_ring_event;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

const char *calls_state_name(call_state_t s)
{
    static const char *n[] = { "idle", "ringing", "dialing", "alerting", "active" };
    return (unsigned)s < 5 ? n[s] : "unknown";
}

void calls_set_cb(calls_cb_t cb) { s_cb = cb; }

void calls_get(call_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_call;
    xSemaphoreGive(s_lock);
}

static cJSON *info_json(const call_info_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", calls_state_name(c->state));
    if (c->state != CALL_IDLE) {
        cJSON_AddStringToObject(o, "direction", c->incoming ? "incoming" : "outgoing");
        cJSON_AddStringToObject(o, "number", c->number);
        if (c->state == CALL_ACTIVE)
            cJSON_AddNumberToObject(o, "duration_s", (double)((now_ms() - c->active_since_ms) / 1000));
    }
    return o;
}

cJSON *calls_state_json(void)
{
    call_info_t c;
    calls_get(&c);
    return info_json(&c);
}

// Applies a new state and publishes it. `reason` is set when the call ends.
static void set_state(call_state_t st, bool incoming, const char *number, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    call_info_t old = s_call;
    bool changed = old.state != st || (number && number[0] && strcmp(old.number, number) != 0);
    if (changed) {
        if (old.state == CALL_IDLE && st != CALL_IDLE) s_call.number[0] = 0;
        s_call.state = st;
        if (st != CALL_IDLE) s_call.incoming = incoming;
        if (number && number[0]) strlcpy(s_call.number, number, sizeof(s_call.number));
        if (old.state != st) s_call.since_ms = now_ms();
        if (st == CALL_ACTIVE && old.state != CALL_ACTIVE) s_call.active_since_ms = now_ms();
    }
    call_info_t cur = s_call;
    xSemaphoreGive(s_lock);
    if (!changed) return;

    ESP_LOGI(TAG, "Call %s -> %s (%s %s)", calls_state_name(old.state), calls_state_name(st),
             cur.incoming ? "from" : "to", cur.number[0] ? cur.number : "unknown");
    cJSON *ev;
    if (st == CALL_IDLE) {
        ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "state", "idle");
        cJSON_AddStringToObject(ev, "direction", old.incoming ? "incoming" : "outgoing");
        cJSON_AddStringToObject(ev, "number", old.number);
        cJSON_AddStringToObject(ev, "previous", calls_state_name(old.state));
        cJSON_AddStringToObject(ev, "reason", reason ? reason : "ended");
        if (old.state == CALL_ACTIVE)
            cJSON_AddNumberToObject(ev, "duration_s", (double)((now_ms() - old.active_since_ms) / 1000));
        if (old.state == CALL_RINGING && old.incoming) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "number", old.number);
            events_publish("missed_call", m);
        }
    } else {
        ev = info_json(&cur);
    }
    events_publish("call_state", ev);
    if (s_cb) s_cb(&cur);
}

// "+CLIP: "+33612345678",145,,,,0"  or  +CLCC: 1,1,4,0,0,"+33612345678",145
static void quoted(const char *s, char *out, size_t len)
{
    out[0] = 0;
    const char *a = strchr(s, '"');
    if (!a) return;
    const char *b = strchr(a + 1, '"');
    if (!b) return;
    size_t n = b - a - 1;
    if (n >= len) n = len - 1;
    memcpy(out, a + 1, n);
    out[n] = 0;
}

static void handle_clcc(const char *line)
{
    // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,<number>,<type>]
    int id, dir, stat, mode;
    if (sscanf(line, "+CLCC: %d,%d,%d,%d", &id, &dir, &stat, &mode) != 4 || mode != 0) return;   // voice only
    char num[24];
    quoted(line, num, sizeof(num));
    bool incoming = dir == 1;
    switch (stat) {
    case 0: set_state(CALL_ACTIVE, incoming, num, NULL); break;
    case 2: set_state(CALL_DIALING, false, num, NULL); break;
    case 3: set_state(CALL_ALERTING, false, num, NULL); break;
    case 4: case 5: set_state(CALL_RINGING, true, num, NULL); break;
    case 6: {
        call_info_t c;
        calls_get(&c);
        set_state(CALL_IDLE, incoming, num, c.state == CALL_RINGING ? "missed" : "ended");
        break;
    }
    default: break;   // held: not used
    }
}

static void handle_urc(const char *line)
{
    call_info_t c;
    calls_get(&c);
    if (strncmp(line, "+CLCC:", 6) == 0) {
        handle_clcc(line);
    } else if (strcmp(line, "RING") == 0 || strncmp(line, "+CLIP:", 6) == 0) {
        char num[24] = "";
        if (line[0] == '+') quoted(line, num, sizeof(num));
        if (c.state == CALL_IDLE || c.state == CALL_RINGING) set_state(CALL_RINGING, true, num, NULL);
        // "ring" event once per ring burst (RING + +CLIP come every ~5 s), with the caller ID when known
        if (line[0] == '+' && now_ms() - s_last_ring_event > 3000) {
            s_last_ring_event = now_ms();
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "number", num);
            events_publish("ring", e);
        }
    } else if (strncmp(line, "VOICE CALL: BEGIN", 17) == 0) {
        if (c.state != CALL_IDLE) set_state(CALL_ACTIVE, c.incoming, NULL, NULL);
    } else if (strncmp(line, "VOICE CALL: END", 15) == 0 || strcmp(line, "NO CARRIER") == 0) {
        set_state(CALL_IDLE, c.incoming, NULL, c.state == CALL_RINGING ? "missed" : "ended");
    } else if (strcmp(line, "BUSY") == 0) {
        set_state(CALL_IDLE, c.incoming, NULL, "busy");
    } else if (strcmp(line, "NO ANSWER") == 0) {
        set_state(CALL_IDLE, c.incoming, NULL, "no_answer");
    }
}

// Runs in the modem RX task: copy and hand over, never block.
static void on_urc(const char *line)
{
    if (strncmp(line, "+CLCC:", 6) && strcmp(line, "RING") && strncmp(line, "+CLIP:", 6) &&
        strncmp(line, "VOICE CALL:", 11) && strcmp(line, "NO CARRIER") && strcmp(line, "BUSY") &&
        strcmp(line, "NO ANSWER"))
        return;
    char buf[URC_LEN];
    strlcpy(buf, line, sizeof(buf));
    xQueueSend(s_urcs, buf, 0);
}

// Asks the modem for the current calls: the source of truth after a reconnect or a missed URC.
static void resync(void)
{
    char resp[512];
    if (modem_cmd("AT+CLCC", "+CLCC:", resp, sizeof(resp), 3000) != ESP_OK) return;
    if (!resp[0]) {
        call_info_t c;
        calls_get(&c);
        if (c.state != CALL_IDLE) set_state(CALL_IDLE, c.incoming, NULL, "ended");
        return;
    }
    char *save = NULL;
    for (char *l = strtok_r(resp, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) handle_clcc(l);
}

static void calls_task(void *arg)
{
    char line[URC_LEN];
    unsigned seen_gen = 0;
    int64_t last_resync = 0;
    while (1) {
        bool got = xQueueReceive(s_urcs, line, pdMS_TO_TICKS(1000)) == pdTRUE;
        if (got) handle_urc(line);

        modem_status_t st;
        modem_get_status(&st);
        if (!st.ready) {
            call_info_t c;
            calls_get(&c);
            if (c.state != CALL_IDLE) set_state(CALL_IDLE, c.incoming, NULL, "modem_down");
            continue;
        }
        if (st.generation != seen_gen) {     // modem (re)started: turn on call reports
            seen_gen = st.generation;
            modem_cmd("AT+CLCC=1", NULL, NULL, 0, 2000);
            resync();
            last_resync = now_ms();
        }
        // While a call is up, double-check every 10 s (a missed "end" would leave us stuck).
        call_info_t c;
        calls_get(&c);
        if (!got && c.state != CALL_IDLE && now_ms() - last_resync > 10000) {
            resync();
            last_resync = now_ms();
        }
    }
}

static bool valid_number(const char *n)
{
    if (*n == '+') n++;
    if (!*n || strlen(n) > 20) return false;
    for (; *n; n++) if (!isdigit((unsigned char)*n) && *n != '*' && *n != '#') return false;
    return true;
}

esp_err_t calls_dial(const char *number, const char **err)
{
    call_info_t c;
    calls_get(&c);
    if (!number || !valid_number(number)) { *err = "bad number (digits, * or #, optional leading +)"; return ESP_ERR_INVALID_ARG; }
    if (c.state != CALL_IDLE) { *err = "a call is already in progress"; return ESP_ERR_INVALID_STATE; }
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "ATD%s;", number);
    set_state(CALL_DIALING, false, number, NULL);
    char resp[96];
    esp_err_t e = modem_cmd(cmd, NULL, resp, sizeof(resp), 20000);
    if (e != ESP_OK) {
        *err = e == ESP_ERR_TIMEOUT ? "modem timeout" : "modem refused the call";
        set_state(CALL_IDLE, false, NULL, "failed");
    }
    return e;
}

esp_err_t calls_answer(const char **err)
{
    call_info_t c;
    calls_get(&c);
    if (c.state != CALL_RINGING) { *err = "no incoming call"; return ESP_ERR_INVALID_STATE; }
    esp_err_t e = modem_cmd("ATA", NULL, NULL, 0, 10000);
    if (e != ESP_OK) *err = "modem refused ATA";
    return e;
}

esp_err_t calls_hangup(const char **err)
{
    call_info_t c;
    calls_get(&c);
    if (c.state == CALL_IDLE) { *err = "no call"; return ESP_ERR_INVALID_STATE; }
    esp_err_t e = modem_cmd("AT+CHUP", NULL, NULL, 0, 10000);
    if (e != ESP_OK) *err = "modem refused AT+CHUP";
    else set_state(CALL_IDLE, c.incoming, NULL, c.state == CALL_RINGING ? "rejected" : "hangup");
    return e;
}

void calls_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_urcs = xQueueCreate(16, URC_LEN);
    modem_add_urc_cb(on_urc);
    xTaskCreatePinnedToCore(calls_task, "calls", 6144, NULL, 6, NULL, 1);
}
