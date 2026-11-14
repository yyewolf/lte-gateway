#include "voice.h"
#include "board.h"
#include "net.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "microlink.h"
#include "g711.h"

static const char *TAG = "voice";

#define FRAME_SAMPLES (AUDIO_RATE_HZ / 50)   // 160 samples = 20 ms
#define RTP_HDR 12
#define JB_FRAMES 8                          // downlink ring: 160 ms max
#define JB_PREFILL 3                         // start playing once 60 ms are queued

static i2c_master_bus_handle_t s_i2c;
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_codec;
static bool s_codec_ok;

static microlink_udp_socket_t *s_sock;
static volatile uint32_t s_target_ip;
static volatile uint16_t s_target_port = VOICE_RTP_PORT;
static volatile bool s_running;
static TaskHandle_t s_task;

// Downlink jitter buffer: decoded frames, written by the UDP RX callback (core 1),
// read by the audio task (core 0). Protected by a spinlock: both sides are short.
static int16_t s_jb[JB_FRAMES][FRAME_SAMPLES];
static uint16_t s_jb_seq[JB_FRAMES];
static int s_jb_head, s_jb_count;
static bool s_jb_playing;
static bool s_have_last_seq;
static uint16_t s_last_seq;
static portMUX_TYPE s_jb_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    uint32_t tx_packets, rx_packets, rx_late, rx_overflow, underruns, tx_errors;
    int64_t started_us;
} s_stats;

// ---------- codec ----------
esp_err_t voice_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_i2c));

    // ESP32 is the I2S master and drives MCLK = 256 x fs; the ES8311 is the slave.
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    cc.dma_desc_num = 4;
    cc.dma_frame_num = FRAME_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, &s_rx));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_LRCK,
                      .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &sc));

    audio_codec_i2s_cfg_t ic = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t ec = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = true,
        .digital_mic = false,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&ec);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 did not answer on I2C: calls will have no audio");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec_if, .data_if = data_if };
    s_codec = esp_codec_dev_new(&dc);
    esp_codec_dev_sample_info_t fs = { .sample_rate = AUDIO_RATE_HZ, .channel = 1, .bits_per_sample = 16,
                                       .mclk_multiple = 256 };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return ESP_FAIL;
    }
    char v[16];
    float in_db = settings_get("audio_in_db", v, sizeof(v)) ? atof(v) : 18;     // bring-up: good downlink level
    int out_vol = settings_get("audio_out_vol", v, sizeof(v)) ? atoi(v) : 70;
    voice_set_gains(in_db, out_vol);
    s_codec_ok = true;
    ESP_LOGI(TAG, "ES8311 ready, %d Hz mono, in %.0f dB, out %d", AUDIO_RATE_HZ, in_db, out_vol);
    return ESP_OK;
}

void voice_set_gains(float in_db, int out_vol)
{
    if (!s_codec) return;
    esp_codec_dev_set_in_gain(s_codec, in_db);
    esp_codec_dev_set_out_vol(s_codec, out_vol);
}

// ---------- RTP in (core 1, microlink's UDP RX task) ----------
static void on_rtp(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port, const uint8_t *d, size_t len,
                   void *ud)
{
    if (!s_running || len < RTP_HDR || (d[0] >> 6) != 2) return;
    if (s_target_ip && src_ip != s_target_ip) return;          // only our relay server
    int pt = d[1] & 0x7F;
    if (pt != 0 && pt != 8) return;                             // PCMU / PCMA
    size_t off = RTP_HDR + 4 * (d[0] & 0x0F);
    if (d[0] & 0x10) {                                          // header extension
        if (len < off + 4) return;
        off += 4 + 4 * ((d[off + 2] << 8) | d[off + 3]);
    }
    if (len < off) return;
    size_t n = len - off;
    if (d[0] & 0x20 && n > 0) n -= d[len - 1];                  // padding
    if (n > FRAME_SAMPLES) n = FRAME_SAMPLES;
    uint16_t seq = (d[2] << 8) | d[3];

    int16_t frame[FRAME_SAMPLES] = { 0 };
    for (size_t i = 0; i < n; i++) frame[i] = pt == 0 ? g711_ulaw2lin(d[off + i]) : g711_alaw2lin(d[off + i]);

    portENTER_CRITICAL(&s_jb_mux);
    s_stats.rx_packets++;
    if (s_have_last_seq && (int16_t)(seq - s_last_seq) <= 0) {
        s_stats.rx_late++;                                      // duplicate or older than what we queued
    } else {
        if (s_jb_count == JB_FRAMES) {                          // full: drop the oldest
            s_jb_head = (s_jb_head + 1) % JB_FRAMES;
            s_jb_count--;
            s_stats.rx_overflow++;
        }
        int slot = (s_jb_head + s_jb_count) % JB_FRAMES;
        memcpy(s_jb[slot], frame, sizeof(frame));
        s_jb_seq[slot] = seq;
        s_jb_count++;
        s_last_seq = seq;
        s_have_last_seq = true;
    }
    portEXIT_CRITICAL(&s_jb_mux);
}

// Next downlink frame, or silence. Returns false on silence.
static bool jb_pop(int16_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_jb_mux);
    if (!s_jb_playing && s_jb_count >= JB_PREFILL) s_jb_playing = true;
    if (s_jb_playing && s_jb_count > 0) {
        memcpy(out, s_jb[s_jb_head], FRAME_SAMPLES * sizeof(int16_t));
        s_jb_head = (s_jb_head + 1) % JB_FRAMES;
        s_jb_count--;
        ok = true;
    } else if (s_jb_playing) {
        s_jb_playing = false;                                   // ran dry: refill before playing again
        s_stats.underruns++;
    }
    portEXIT_CRITICAL(&s_jb_mux);
    if (!ok) memset(out, 0, FRAME_SAMPLES * sizeof(int16_t));
    return ok;
}

// ---------- audio loop (core 0) ----------
static void audio_task(void *arg)
{
    static int16_t up[FRAME_SAMPLES], down[FRAME_SAMPLES];
    static uint8_t pkt[RTP_HDR + FRAME_SAMPLES];
    uint16_t seq = esp_random();
    uint32_t ts = esp_random(), ssrc = esp_random();
    bool first = true;

    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    while (s_running) {
        size_t got = 0;
        // Blocks for one 20 ms frame: the I2S clock paces the whole loop.
        if (i2s_channel_read(s_rx, up, sizeof(up), &got, pdMS_TO_TICKS(100)) != ESP_OK || got != sizeof(up)) continue;

        uint32_t ip = s_target_ip;
        if (ip && s_sock) {
            pkt[0] = 0x80;
            pkt[1] = (first ? 0x80 : 0) | 0;                    // marker on the first packet, PT 0 = PCMU
            pkt[2] = seq >> 8; pkt[3] = seq;
            pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
            pkt[8] = ssrc >> 24; pkt[9] = ssrc >> 16; pkt[10] = ssrc >> 8; pkt[11] = ssrc;
            for (int i = 0; i < FRAME_SAMPLES; i++) pkt[RTP_HDR + i] = g711_lin2ulaw(up[i]);
            if (microlink_udp_send(s_sock, ip, s_target_port, pkt, sizeof(pkt)) == ESP_OK) s_stats.tx_packets++;
            else s_stats.tx_errors++;
            first = false;
        }
        seq++;
        ts += FRAME_SAMPLES;

        jb_pop(down);
        size_t wrote = 0;
        i2s_channel_write(s_tx, down, sizeof(down), &wrote, pdMS_TO_TICKS(100));
    }
    i2s_channel_disable(s_rx);
    i2s_channel_disable(s_tx);
    s_task = NULL;
    vTaskDelete(NULL);
}

void voice_set_target(uint32_t ip, uint16_t port)
{
    s_target_ip = ip;
    s_target_port = port ? port : VOICE_RTP_PORT;
    char s[16];
    microlink_ip_to_str(ip, s);
    ESP_LOGI(TAG, "RTP target %s:%u", s, s_target_port);
}

void voice_start(void)
{
    if (!s_codec_ok || s_running) return;
    if (!s_sock) {
        microlink_t *ml = net_microlink();
        if (ml && microlink_is_connected(ml)) {
            s_sock = microlink_udp_create(ml, VOICE_RTP_PORT);
            if (s_sock) microlink_udp_set_rx_callback(s_sock, on_rtp, NULL);
        }
        if (!s_sock) ESP_LOGW(TAG, "No tailnet UDP socket: the call has no audio relay");
    }
    if (!s_target_ip) ESP_LOGW(TAG, "No RTP target yet (no server connected to /events)");
    portENTER_CRITICAL(&s_jb_mux);
    s_jb_head = s_jb_count = 0;
    s_jb_playing = false;
    s_have_last_seq = false;
    portEXIT_CRITICAL(&s_jb_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.started_us = esp_timer_get_time();
    s_running = true;
    // Core 0, just below the WiFi task: nothing else time-critical lives there.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, configMAX_PRIORITIES - 3, &s_task, 0);
    ESP_LOGI(TAG, "Audio relay started");
}

void voice_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "Audio relay stopped: sent %lu, received %lu, late %lu, underruns %lu",
             (unsigned long)s_stats.tx_packets, (unsigned long)s_stats.rx_packets, (unsigned long)s_stats.rx_late,
             (unsigned long)s_stats.underruns);
}

cJSON *voice_stats_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "running", s_running);
    char ip[16] = "";
    if (s_target_ip) microlink_ip_to_str(s_target_ip, ip);
    cJSON_AddStringToObject(o, "target", ip);
    cJSON_AddNumberToObject(o, "target_port", s_target_port);
    cJSON_AddNumberToObject(o, "tx_packets", s_stats.tx_packets);
    cJSON_AddNumberToObject(o, "tx_errors", s_stats.tx_errors);
    cJSON_AddNumberToObject(o, "rx_packets", s_stats.rx_packets);
    cJSON_AddNumberToObject(o, "rx_late", s_stats.rx_late);
    cJSON_AddNumberToObject(o, "rx_overflow", s_stats.rx_overflow);
    cJSON_AddNumberToObject(o, "underruns", s_stats.underruns);
    return o;
}
