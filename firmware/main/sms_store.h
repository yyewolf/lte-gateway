// Flash-backed SMS store in its own NVS partition ("sms"): an inbox the server must acknowledge,
// and an outbox that survives reboots and WiFi/modem outages.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SMS_TEXT_MAX 2048          // UTF-8, a 10-part message fits
#define SMS_ADDR_MAX 24

typedef struct {
    uint32_t id;
    char from[SMS_ADDR_MAX];
    char timestamp[32];            // service centre time, ISO 8601
    uint8_t parts;
    bool complete;                 // false: some parts never arrived
    char *text;                    // heap; free with sms_msg_free
} sms_in_t;

typedef enum { SMS_OUT_QUEUED = 0, SMS_OUT_SENT, SMS_OUT_FAILED } sms_out_state_t;

typedef struct {
    uint32_t id;
    char to[SMS_ADDR_MAX];
    uint8_t state;                 // sms_out_state_t
    uint8_t parts, sent_parts, attempts;
    uint8_t ref;                   // concatenation reference
    int16_t last_mr;               // message reference of the last part, -1 if none
    char error[48];
    char *text;
} sms_out_t;

esp_err_t sms_store_init(void);

esp_err_t sms_inbox_add(sms_in_t *msg);           // assigns msg->id
esp_err_t sms_inbox_get(uint32_t id, sms_in_t *out);
esp_err_t sms_inbox_delete(uint32_t id);
// Ids in ascending order, after `after` (0 = from the start). Returns the count (<= max).
int sms_inbox_list(uint32_t after, uint32_t *ids, int max);
int sms_inbox_count(void);

esp_err_t sms_outbox_add(sms_out_t *msg);         // assigns msg->id
esp_err_t sms_outbox_get(uint32_t id, sms_out_t *out);
esp_err_t sms_outbox_update(const sms_out_t *msg);
// Oldest queued message id, 0 if none.
uint32_t sms_outbox_next_queued(void);

void sms_in_free(sms_in_t *m);
void sms_out_free(sms_out_t *m);
