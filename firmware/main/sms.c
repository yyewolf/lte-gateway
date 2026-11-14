#include "sms.h"
#include "sms_store.h"
#include "pdu.h"
#include "modem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "sms";

#define EV_CHECK_INBOX BIT0
#define EV_CHECK_OUTBOX BIT1
#define MAX_ON_MODEM 60                 // messages read per pass
#define CONCAT_WAIT_US (60LL * 60 * 1000000)   // deliver an incomplete long SMS after 1 h
#define SEND_RETRIES 3
#define LIST_BUF (48 * 1024)

static EventGroupHandle_t s_ev;
static sms_event_cb_t s_event_cb;

typedef struct {
    int index;                          // position in modem memory
    pdu_deliver_t pdu;
} modem_msg_t;

// First time we saw an incomplete group (sender + ref), to time it out. RAM only.
typedef struct { char sender[24]; uint16_t ref; int64_t first_seen; } pending_t;
static pending_t s_pending[16];

static void emit(const char *ev, uint32_t id) { if (s_event_cb) s_event_cb(ev, id); }
void sms_set_event_cb(sms_event_cb_t cb) { s_event_cb = cb; }
void sms_kick(void) { if (s_ev) xEventGroupSetBits(s_ev, EV_CHECK_INBOX | EV_CHECK_OUTBOX); }

static void on_urc(const char *line)
{
    if (strncmp(line, "+CMTI:", 6) == 0 || strncmp(line, "+CDSI:", 6) == 0)
        xEventGroupSetBits(s_ev, EV_CHECK_INBOX);
}

static int64_t pending_age(const char *sender, uint16_t ref)
{
    int64_t now = esp_timer_get_time();
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < 16; i++) {
        if (s_pending[i].first_seen && s_pending[i].ref == ref && strcmp(s_pending[i].sender, sender) == 0)
            return now - s_pending[i].first_seen;
        if (!s_pending[i].first_seen && free_slot < 0) free_slot = i;
        if (s_pending[i].first_seen < s_pending[oldest].first_seen) oldest = i;
    }
    int slot = free_slot >= 0 ? free_slot : oldest;
    strlcpy(s_pending[slot].sender, sender, sizeof(s_pending[slot].sender));
    s_pending[slot].ref = ref;
    s_pending[slot].first_seen = now;
    return 0;
}

static void pending_clear(const char *sender, uint16_t ref)
{
    for (int i = 0; i < 16; i++)
        if (s_pending[i].first_seen && s_pending[i].ref == ref && strcmp(s_pending[i].sender, sender) == 0)
            s_pending[i].first_seen = 0;
}

static void delete_on_modem(int index)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    modem_cmd(cmd, NULL, NULL, 0, 5000);
}

// Stores one message (single or the parts of a long one, in order) and deletes it from the modem.
static bool store(modem_msg_t **parts, int n, int total, bool complete)
{
    sms_in_t in = { .parts = total, .complete = complete };
    strlcpy(in.from, parts[0]->pdu.sender, sizeof(in.from));
    strlcpy(in.timestamp, parts[0]->pdu.timestamp, sizeof(in.timestamp));
    in.text = calloc(1, SMS_TEXT_MAX);
    if (!in.text) return false;
    for (int i = 0; i < n; i++) strlcat(in.text, parts[i]->pdu.text, SMS_TEXT_MAX);
    esp_err_t err = sms_inbox_add(&in);
    free(in.text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Inbox full or flash error (%s): leaving the message on the modem", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Received #%lu from %s (%d/%d parts)", (unsigned long)in.id, in.from, n, total);
    for (int i = 0; i < n; i++) delete_on_modem(parts[i]->index);
    emit("sms_received", in.id);
    return true;
}

static int cmp_seq(const void *a, const void *b)
{
    const modem_msg_t *x = *(modem_msg_t *const *)a, *y = *(modem_msg_t *const *)b;
    return x->pdu.concat_seq - y->pdu.concat_seq;
}

static void check_inbox(void)
{
    char *buf = heap_caps_malloc(LIST_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    modem_msg_t *msgs = heap_caps_calloc(MAX_ON_MODEM, sizeof(modem_msg_t), MALLOC_CAP_SPIRAM);
    if (!buf || !msgs) { free(buf); free(msgs); return; }

    // All messages, any state. Lines: "+CMGL: <index>,<stat>,,<len>" then the PDU.
    if (modem_cmd("AT+CMGL=4", NULL, buf, LIST_BUF, 20000) != ESP_OK) {
        free(buf); free(msgs);
        return;
    }
    int n = 0, index = -1;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && n < MAX_ON_MODEM; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "+CMGL:", 6) == 0) { index = atoi(line + 6); continue; }
        if (index < 0) continue;
        msgs[n].index = index;
        if (pdu_decode(line, &msgs[n].pdu) != 0) {
            ESP_LOGW(TAG, "Undecodable PDU at index %d, deleting it: %.60s", index, line);
            delete_on_modem(index);
        } else if (msgs[n].pdu.status_report) {
            delete_on_modem(index);          // we don't request reports; drop strays
        } else n++;
        index = -1;
    }
    free(buf);

    bool used[MAX_ON_MODEM] = { 0 };
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        modem_msg_t *m = &msgs[i];
        if (!m->pdu.concat) {
            used[i] = true;
            modem_msg_t *one = m;
            store(&one, 1, 1, true);
            continue;
        }
        // Gather the parts of this long message (same sender + reference + total).
        modem_msg_t *group[PDU_MAX_PARTS * 3];
        int g = 0;
        bool seen[256] = { 0 };
        for (int j = i; j < n; j++) {
            pdu_deliver_t *p = &msgs[j].pdu;
            if (used[j] || !p->concat || p->concat_ref != m->pdu.concat_ref || p->concat_total != m->pdu.concat_total ||
                strcmp(p->sender, m->pdu.sender) != 0)
                continue;
            used[j] = true;
            if (seen[p->concat_seq]) { delete_on_modem(msgs[j].index); continue; }   // duplicate part
            seen[p->concat_seq] = true;
            if (g < (int)(sizeof(group) / sizeof(group[0]))) group[g++] = &msgs[j];
        }
        qsort(group, g, sizeof(group[0]), cmp_seq);
        int total = m->pdu.concat_total;
        if (g == total) {
            if (store(group, g, total, true)) pending_clear(m->pdu.sender, m->pdu.concat_ref);
        } else if (pending_age(m->pdu.sender, m->pdu.concat_ref) > CONCAT_WAIT_US) {
            ESP_LOGW(TAG, "Long SMS from %s still missing parts after 1 h: delivering %d/%d", m->pdu.sender, g, total);
            if (store(group, g, total, false)) pending_clear(m->pdu.sender, m->pdu.concat_ref);
        } else {
            ESP_LOGI(TAG, "Long SMS from %s: %d/%d parts so far", m->pdu.sender, g, total);
        }
    }
    free(msgs);
}

static void send_one(uint32_t id)
{
    sms_out_t m;
    if (sms_outbox_get(id, &m) != ESP_OK) return;
    static pdu_part_t parts[PDU_MAX_PARTS];
    int np = pdu_encode(m.to, m.text, m.ref, parts, PDU_MAX_PARTS);
    if (np <= 0) {
        m.state = SMS_OUT_FAILED;
        strlcpy(m.error, "cannot encode", sizeof(m.error));
    } else {
        m.attempts++;
        char resp[64];
        while (m.sent_parts < np) {
            pdu_part_t *p = &parts[m.sent_parts];
            esp_err_t err = modem_send_pdu(p->tpdu_len, p->hex, resp, sizeof(resp), 60000);
            if (err != ESP_OK) {
                strlcpy(m.error, resp[0] ? resp : esp_err_to_name(err), sizeof(m.error));
                break;
            }
            const char *c = strchr(resp, ':');
            m.last_mr = c ? atoi(c + 1) : -1;
            m.sent_parts++;
            sms_outbox_update(&m);           // a reboot mid-message resumes at the next part
        }
        if (m.sent_parts == np) {
            m.state = SMS_OUT_SENT;
            m.error[0] = 0;
        } else if (m.attempts >= SEND_RETRIES) {
            m.state = SMS_OUT_FAILED;
        }
    }
    sms_outbox_update(&m);
    if (m.state == SMS_OUT_SENT) {
        ESP_LOGI(TAG, "Sent #%lu to %s (%d parts)", (unsigned long)m.id, m.to, np);
        emit("sms_sent", m.id);
    } else if (m.state == SMS_OUT_FAILED) {
        ESP_LOGE(TAG, "Giving up on #%lu to %s: %s", (unsigned long)m.id, m.to, m.error);
        emit("sms_failed", m.id);
    } else {
        ESP_LOGW(TAG, "Send #%lu failed (%s), will retry", (unsigned long)m.id, m.error);
    }
    sms_out_free(&m);
}

static bool modem_usable(unsigned *gen)
{
    modem_status_t st;
    modem_get_status(&st);
    *gen = st.generation;
    return st.ready && st.sim[0] && strcmp(st.sim, "READY") == 0;
}

static void sms_task(void *arg)
{
    unsigned seen_gen = 0, gen;
    int64_t last_retry = 0;
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_ev, EV_CHECK_INBOX | EV_CHECK_OUTBOX, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(60000));
        if (!modem_usable(&gen)) continue;
        bool fresh = gen != seen_gen;        // modem just (re)started: catch up on everything
        seen_gen = gen;
        // Poll every minute as well: covers a missed +CMTI and times out incomplete long SMS.
        check_inbox();

        int64_t now = esp_timer_get_time();
        bool retry_due = now - last_retry > 30LL * 1000000;
        if (fresh || (bits & EV_CHECK_OUTBOX) || retry_due) {
            last_retry = now;
            for (int i = 0; i < 20; i++) {   // drain, but let the inbox breathe
                uint32_t id = sms_outbox_next_queued();
                if (!id) break;
                sms_out_t before;
                if (sms_outbox_get(id, &before) != ESP_OK) break;
                uint8_t att = before.attempts;
                sms_out_free(&before);
                send_one(id);
                sms_out_t after;
                if (sms_outbox_get(id, &after) == ESP_OK) {
                    bool stuck = after.state == SMS_OUT_QUEUED && after.attempts > att;
                    sms_out_free(&after);
                    if (stuck) break;        // failed this round: wait for the next retry
                }
            }
        }
    }
}

esp_err_t sms_send(const char *to, const char *text, uint32_t *id, int *parts, bool *ucs2, const char **err)
{
    pdu_part_t probe[1];
    *err = NULL;
    if (!to || !text) { *err = "need 'to' and 'text'"; return ESP_ERR_INVALID_ARG; }
    if (strlen(text) >= SMS_TEXT_MAX) { *err = "text too long"; return ESP_ERR_INVALID_ARG; }
    int np = pdu_count_parts(text, ucs2);
    if (np < 0) { *err = "text too long (max 10 parts) or not valid UTF-8"; return ESP_ERR_INVALID_ARG; }
    if (pdu_encode(to, "x", 0, probe, 1) != 1) { *err = "bad phone number (digits, optional leading +)"; return ESP_ERR_INVALID_ARG; }
    sms_out_t m = { .state = SMS_OUT_QUEUED, .parts = np, .ref = esp_random() & 0xFF, .last_mr = -1,
                    .text = (char *)text };
    strlcpy(m.to, to, sizeof(m.to));
    esp_err_t e = sms_outbox_add(&m);
    if (e != ESP_OK) { *err = "flash error"; return e; }
    *id = m.id;
    *parts = np;
    xEventGroupSetBits(s_ev, EV_CHECK_OUTBOX);
    return ESP_OK;
}

void sms_start(void)
{
    s_ev = xEventGroupCreate();
    sms_store_init();
    modem_add_urc_cb(on_urc);
    xTaskCreatePinnedToCore(sms_task, "sms", 8192, NULL, 5, NULL, 1);
}
