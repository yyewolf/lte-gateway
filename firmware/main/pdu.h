// SMS PDU encoding (SMS-SUBMIT) and decoding (SMS-DELIVER), 3GPP TS 23.040 / 23.038.
// Pure C, no ESP-IDF dependency, so it can be unit-tested on a PC (see test/).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PDU_TEXT_MAX   512   // UTF-8 text of one part
#define PDU_HEX_MAX    400   // hex string of one SMS-SUBMIT (SMSC byte + TPDU)
#define PDU_MAX_PARTS  10    // longest message we send: 10 parts (1530 GSM-7 / 670 UCS-2 chars)

typedef struct {
    char sender[24];          // "+33612345678", or an alphanumeric name like "ORANGE"
    char timestamp[32];       // ISO 8601 service-centre time, "2026-11-14T16:55:12+01:00"
    char text[PDU_TEXT_MAX];  // UTF-8
    bool concat;              // part of a long message
    uint16_t concat_ref;
    uint8_t concat_total, concat_seq;   // seq is 1-based
    bool binary;              // 8-bit data (text holds hex)
    bool status_report;       // SMS-STATUS-REPORT, not a message
} pdu_deliver_t;

typedef struct {
    char hex[PDU_HEX_MAX];    // what to send after AT+CMGS=<tpdu_len>
    int tpdu_len;             // octets after the SMSC field
} pdu_part_t;

// Decode one PDU as returned by AT+CMGR / AT+CMGL in PDU mode. Returns 0 on success.
int pdu_decode(const char *hex, pdu_deliver_t *out);

// Encode a UTF-8 message to `to` ("+336...", or a national number). Uses GSM-7 when every
// character fits, UCS-2 otherwise; splits into concatenated parts with reference `ref`.
// Returns the number of parts (1..max_parts), or -1 if the text is too long / invalid.
int pdu_encode(const char *to, const char *utf8, uint8_t ref, pdu_part_t *parts, int max_parts);

// Number of parts pdu_encode would produce, -1 if too long. `ucs2` tells which alphabet.
int pdu_count_parts(const char *utf8, bool *ucs2);
