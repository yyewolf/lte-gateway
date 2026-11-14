// SMS engine: pulls received messages off the modem into the inbox (reassembling long ones)
// and sends the outbox.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*sms_event_cb_t)(const char *event, uint32_t id);  // "sms_received", "sms_sent", "sms_failed"

void sms_start(void);
void sms_set_event_cb(sms_event_cb_t cb);
// Validates and queues a message. Returns ESP_OK and the id, ESP_ERR_INVALID_ARG for a bad
// number/too long text (with a reason in err).
esp_err_t sms_send(const char *to, const char *text, uint32_t *id, int *parts, bool *ucs2, const char **err);
void sms_kick(void);   // check the modem and the outbox now
