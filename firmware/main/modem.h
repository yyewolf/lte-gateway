// A7670E/A7683E over UART: power sequencing, a serialized AT command engine, unsolicited
// result codes (URCs), and a manager task that powers the modem up and keeps a status snapshot.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool powered;          // supply switch on
    bool ready;            // answering AT
    char sim[16];          // READY, SIM PIN, NOT INSERTED, ...
    int rssi_dbm;          // from +CSQ, 0 = unknown
    int csq;               // raw +CSQ value, 99 = unknown
    int reg;               // +CEREG stat: 1 home, 5 roaming, 2 searching, 3 denied, 0 not searching
    char operator_name[32];
    char imei[20];
    char iccid[24];
    char revision[48];
    float vbat;            // volts at the modem
    unsigned at_errors;    // consecutive AT timeouts
    unsigned generation;   // +1 each time the modem has been (re)configured after power-up
} modem_status_t;

// Called from the modem RX task for every unsolicited line (RING, +CLIP: ..., +CMTI: ...).
// Keep it short; never call modem_cmd() from it.
typedef void (*modem_urc_cb_t)(const char *line);

void modem_init(void);                       // UART + RX task. Leaves the modem as it is.
void modem_start(void);                      // manager task: power on, configure, poll status
esp_err_t modem_power_on(void);
esp_err_t modem_power_off(void);             // AT+CPOF, wait for STATUS low, supply off
void modem_add_urc_cb(modem_urc_cb_t cb);   // up to 4 listeners
void modem_get_status(modem_status_t *out);

// Send one AT command and wait for OK / ERROR / "> ".
// Lines starting with `prefix` (or every non-final line when prefix is NULL) are copied to `resp`,
// newline-separated. resp may be NULL. Returns ESP_OK on OK, ESP_FAIL on ERROR, ESP_ERR_TIMEOUT.
esp_err_t modem_cmd(const char *cmd, const char *prefix, char *resp, size_t resp_len, int timeout_ms);

// Send one SMS-SUBMIT PDU: AT+CMGS=<tpdu_len>, wait for "> ", send hex + Ctrl-Z, wait for OK.
// The "+CMGS: <mr>" line goes to resp. Holds the command lock for the whole exchange.
esp_err_t modem_send_pdu(int tpdu_len, const char *hex, char *resp, size_t resp_len, int timeout_ms);
