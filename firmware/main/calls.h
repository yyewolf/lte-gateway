// Voice call control and state, from the modem's +CLCC / RING / +CLIP URCs.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum {
    CALL_IDLE = 0,
    CALL_RINGING,      // incoming, not answered yet
    CALL_DIALING,      // outgoing, before the other side rings
    CALL_ALERTING,     // outgoing, the other side rings
    CALL_ACTIVE,
} call_state_t;

typedef struct {
    call_state_t state;
    bool incoming;
    char number[24];          // caller ID / dialled number, "" if withheld
    long long since_ms;       // uptime when this state started
    long long active_since_ms;
} call_info_t;

void calls_start(void);
void calls_get(call_info_t *out);
const char *calls_state_name(call_state_t s);
cJSON *calls_state_json(void);              // {"state": ..., "direction": ..., "number": ..., ...}

esp_err_t calls_dial(const char *number, const char **err);
esp_err_t calls_answer(const char **err);
esp_err_t calls_hangup(const char **err);

// Called on every state change (from the calls task). Step 4 hooks the audio path here.
typedef void (*calls_cb_t)(const call_info_t *info);
void calls_set_cb(calls_cb_t cb);
