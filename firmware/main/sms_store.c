#include "sms_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "sms_store";

#define PART "sms"
#define NS_IN "inbox"
#define NS_OUT "outbox"
#define NS_META "meta"
#define OUTBOX_KEEP 50             // sent/failed messages kept for status queries

static SemaphoreHandle_t s_lock;

typedef struct __attribute__((packed)) {
    uint32_t id;
    char from[SMS_ADDR_MAX];
    char timestamp[32];
    uint8_t parts, complete;
} in_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    char to[SMS_ADDR_MAX];
    uint8_t state, parts, sent_parts, attempts, ref;
    int16_t last_mr;
    char error[48];
} out_hdr_t;

static void key_of(uint32_t id, char *k) { snprintf(k, 12, "%08lx", (unsigned long)id); }

static uint32_t next_id(nvs_handle_t meta, const char *name)
{
    uint32_t v = 0;
    nvs_get_u32(meta, name, &v);
    v++;
    nvs_set_u32(meta, name, v);
    nvs_commit(meta);
    return v;
}

esp_err_t sms_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "SMS store unreadable, erasing it");
        nvs_flash_erase_partition(PART);
        err = nvs_flash_init_partition(PART);
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "SMS store ready: %d message(s) in the inbox", sms_inbox_count());
    else ESP_LOGE(TAG, "SMS store init failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t put_blob(const char *ns, uint32_t id, const void *hdr, size_t hl, const char *text)
{
    size_t tl = strlen(text) + 1;
    uint8_t *buf = malloc(hl + tl);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, hdr, hl);
    memcpy(buf + hl, text, tl);
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char k[12];
        key_of(id, k);
        err = nvs_set_blob(h, k, buf, hl + tl);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(buf);
    return err;
}

// Reads a record; *text is malloc'd. hl = header size.
static esp_err_t get_blob(const char *ns, uint32_t id, void *hdr, size_t hl, char **text)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    char k[12];
    key_of(id, k);
    size_t len = 0;
    err = nvs_get_blob(h, k, NULL, &len);
    if (err == ESP_OK && len <= hl) err = ESP_ERR_INVALID_SIZE;
    uint8_t *buf = err == ESP_OK ? malloc(len + 1) : NULL;
    if (err == ESP_OK && !buf) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = nvs_get_blob(h, k, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { free(buf); return err; }
    buf[len] = 0;
    memcpy(hdr, buf, hl);
    *text = strdup((char *)buf + hl);
    free(buf);
    return *text ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t erase_key(const char *ns, uint32_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char k[12];
    key_of(id, k);
    err = nvs_erase_key(h, k);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// All ids of a namespace, sorted ascending. Returns a malloc'd array.
static uint32_t *list_ids(const char *ns, int *count)
{
    int cap = 32, n = 0;
    uint32_t *ids = malloc(cap * sizeof(uint32_t));
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(PART, ns, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && ids) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (n == cap) {
            cap *= 2;
            uint32_t *nids = realloc(ids, cap * sizeof(uint32_t));
            if (!nids) break;
            ids = nids;
        }
        ids[n++] = strtoul(info.key, NULL, 16);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (ids) qsort(ids, n, sizeof(uint32_t), cmp_u32);
    *count = ids ? n : 0;
    return ids;
}

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// ---------- inbox ----------
esp_err_t sms_inbox_add(sms_in_t *m)
{
    LOCK();
    nvs_handle_t meta;
    esp_err_t err = nvs_open_from_partition(PART, NS_META, NVS_READWRITE, &meta);
    if (err == ESP_OK) {
        m->id = next_id(meta, "in_id");
        nvs_close(meta);
        in_hdr_t h = { .id = m->id, .parts = m->parts, .complete = m->complete };
        strlcpy(h.from, m->from, sizeof(h.from));
        strlcpy(h.timestamp, m->timestamp, sizeof(h.timestamp));
        err = put_blob(NS_IN, m->id, &h, sizeof(h), m->text ? m->text : "");
    }
    UNLOCK();
    return err;
}

esp_err_t sms_inbox_get(uint32_t id, sms_in_t *out)
{
    memset(out, 0, sizeof(*out));
    in_hdr_t h;
    LOCK();
    esp_err_t err = get_blob(NS_IN, id, &h, sizeof(h), &out->text);
    UNLOCK();
    if (err != ESP_OK) return err;
    out->id = h.id;
    memcpy(out->from, h.from, sizeof(out->from));
    out->from[SMS_ADDR_MAX - 1] = 0;
    memcpy(out->timestamp, h.timestamp, sizeof(out->timestamp));
    out->timestamp[sizeof(out->timestamp) - 1] = 0;
    out->parts = h.parts;
    out->complete = h.complete;
    return ESP_OK;
}

esp_err_t sms_inbox_delete(uint32_t id)
{
    LOCK();
    esp_err_t err = erase_key(NS_IN, id);
    UNLOCK();
    return err;
}

int sms_inbox_list(uint32_t after, uint32_t *out, int max)
{
    int n, k = 0;
    LOCK();
    uint32_t *ids = list_ids(NS_IN, &n);
    UNLOCK();
    for (int i = 0; i < n && k < max; i++) if (ids[i] > after) out[k++] = ids[i];
    free(ids);
    return k;
}

int sms_inbox_count(void)
{
    int n;
    LOCK();
    free(list_ids(NS_IN, &n));
    UNLOCK();
    return n;
}

// ---------- outbox ----------
static esp_err_t out_put(const sms_out_t *m)
{
    out_hdr_t h = { .id = m->id, .state = m->state, .parts = m->parts, .sent_parts = m->sent_parts,
                    .attempts = m->attempts, .ref = m->ref, .last_mr = m->last_mr };
    strlcpy(h.to, m->to, sizeof(h.to));
    strlcpy(h.error, m->error, sizeof(h.error));
    return put_blob(NS_OUT, m->id, &h, sizeof(h), m->text ? m->text : "");
}

static esp_err_t out_get(uint32_t id, sms_out_t *out)
{
    memset(out, 0, sizeof(*out));
    out_hdr_t h;
    esp_err_t err = get_blob(NS_OUT, id, &h, sizeof(h), &out->text);
    if (err != ESP_OK) return err;
    out->id = h.id;
    memcpy(out->to, h.to, sizeof(out->to));
    out->to[SMS_ADDR_MAX - 1] = 0;
    out->state = h.state;
    out->parts = h.parts;
    out->sent_parts = h.sent_parts;
    out->attempts = h.attempts;
    out->ref = h.ref;
    out->last_mr = h.last_mr;
    memcpy(out->error, h.error, sizeof(out->error));
    out->error[sizeof(out->error) - 1] = 0;
    return ESP_OK;
}

// Drops the oldest finished messages beyond OUTBOX_KEEP.
static void out_prune(void)
{
    int n;
    uint32_t *ids = list_ids(NS_OUT, &n);
    int finished = 0;
    for (int i = n - 1; i >= 0; i--) {
        sms_out_t m;
        if (out_get(ids[i], &m) != ESP_OK) continue;
        bool done = m.state != SMS_OUT_QUEUED;
        sms_out_free(&m);
        if (done && ++finished > OUTBOX_KEEP) erase_key(NS_OUT, ids[i]);
    }
    free(ids);
}

esp_err_t sms_outbox_add(sms_out_t *m)
{
    LOCK();
    nvs_handle_t meta;
    esp_err_t err = nvs_open_from_partition(PART, NS_META, NVS_READWRITE, &meta);
    if (err == ESP_OK) {
        m->id = next_id(meta, "out_id");
        nvs_close(meta);
        err = out_put(m);
    }
    UNLOCK();
    return err;
}

esp_err_t sms_outbox_get(uint32_t id, sms_out_t *out)
{
    LOCK();
    esp_err_t err = out_get(id, out);
    UNLOCK();
    return err;
}

esp_err_t sms_outbox_update(const sms_out_t *m)
{
    LOCK();
    esp_err_t err = out_put(m);
    if (err == ESP_OK && m->state != SMS_OUT_QUEUED) out_prune();
    UNLOCK();
    return err;
}

uint32_t sms_outbox_next_queued(void)
{
    int n;
    uint32_t found = 0;
    LOCK();
    uint32_t *ids = list_ids(NS_OUT, &n);
    for (int i = 0; i < n && !found; i++) {
        out_hdr_t h;
        char *t = NULL;
        if (get_blob(NS_OUT, ids[i], &h, sizeof(h), &t) == ESP_OK && h.state == SMS_OUT_QUEUED) found = ids[i];
        free(t);
    }
    free(ids);
    UNLOCK();
    return found;
}

void sms_in_free(sms_in_t *m) { free(m->text); m->text = NULL; }
void sms_out_free(sms_out_t *m) { free(m->text); m->text = NULL; }
