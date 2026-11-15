#include "console.h"
#include "app.h"
#include "settings.h"
#include "modem.h"
#include "net.h"
#include "board.h"
#include "sms.h"
#include "sms_store.h"
#include "calls.h"
#include "voice.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static void join_args(int argc, char **argv, int from, char *out, size_t len)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) strlcat(out, " ", len);
        strlcat(out, argv[i], len);
    }
}

static int cmd_status(int argc, char **argv)
{
    net_status_t n;
    modem_status_t m;
    net_get_status(&n);
    modem_get_status(&m);
    printf("Firmware %s, up %llu s, free heap %u, free PSRAM %u, VBUS %.2f V\n", GW_VERSION,
           esp_timer_get_time() / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), board_vbus());
    printf("WiFi:    %s \"%s\" %d dBm, LAN IP %s\n", n.wifi_up ? "up" : "down", n.ssid, n.rssi, n.lan_ip);
    printf("Tailnet: %s %s, %d peers\n", n.tailnet_state, n.tailnet_ip, n.peers);
    printf("Modem:   %s%s, VBAT %.2f V, SIM %s, reg %d, %s, CSQ %d (%d dBm)\n", m.powered ? "powered" : "off",
           m.ready ? " + ready" : "", m.vbat, m.sim, m.reg, m.operator_name, m.csq, m.rssi_dbm);
    printf("         IMEI %s, ICCID %s, %s\n", m.imei, m.iccid, m.revision);
    call_info_t c;
    calls_get(&c);
    printf("Call:    %s %s\n", calls_state_name(c.state), c.number);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    char v[160];
    for (size_t i = 0; i < g_settings_count; i++) {
        bool set = settings_get(g_settings[i].key, v, sizeof(v));
        printf("  %-14s = %-30s  %s\n", g_settings[i].key, !set ? "(unset)" : g_settings[i].secret ? "********" : v,
               g_settings[i].help);
    }
    printf("Change with: set <key> <value>   (then 'reboot')\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2 || !settings_find(argv[1])) {
        printf("usage: set <key> [value]   (no value = erase). Keys: see 'config'\n");
        return 1;
    }
    char v[160];
    join_args(argc, argv, 2, v, sizeof(v));
    esp_err_t e = settings_set(argv[1], v);
    printf("%s %s\n", argv[1], e == ESP_OK ? (v[0] ? "saved" : "erased") : esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}

static int cmd_at(int argc, char **argv)
{
    char line[256], resp[1024];
    join_args(argc, argv, 1, line, sizeof(line));
    if (!line[0]) strlcpy(line, "AT", sizeof(line));
    esp_err_t e = modem_cmd(line, NULL, resp, sizeof(resp), 10000);
    if (resp[0]) printf("%s\n", resp);
    printf("%s\n", e == ESP_OK ? "OK" : e == ESP_FAIL ? "ERROR" : "(timeout)");
    return e == ESP_OK ? 0 : 1;
}

static int cmd_sms(int argc, char **argv)
{
    if (argc < 3) { printf("usage: sms <number> <text...>\n"); return 1; }
    char text[1024];
    join_args(argc, argv, 2, text, sizeof(text));
    uint32_t id;
    int parts;
    bool ucs2;
    const char *why;
    if (sms_send(argv[1], text, &id, &parts, &ucs2, &why) != ESP_OK) { printf("Refused: %s\n", why); return 1; }
    printf("Queued as #%lu (%d part(s), %s)\n", (unsigned long)id, parts, ucs2 ? "UCS-2" : "GSM-7");
    return 0;
}

static int cmd_inbox(int argc, char **argv)
{
    uint32_t ids[50];
    int n = sms_inbox_list(0, ids, 50);
    for (int i = 0; i < n; i++) {
        sms_in_t m;
        if (sms_inbox_get(ids[i], &m) != ESP_OK) continue;
        printf("#%lu %s %s%s: %s\n", (unsigned long)m.id, m.timestamp, m.from, m.complete ? "" : " (incomplete)", m.text);
        sms_in_free(&m);
    }
    printf("%d message(s). They stay until your server deletes them (or 'inbox_del <id>').\n", n);
    return 0;
}

static int cmd_inbox_del(int argc, char **argv)
{
    if (argc < 2) { printf("usage: inbox_del <id>\n"); return 1; }
    esp_err_t e = sms_inbox_delete(strtoul(argv[1], NULL, 10));
    printf("%s\n", e == ESP_OK ? "deleted" : "not found");
    return e == ESP_OK ? 0 : 1;
}

static int cmd_dial(int argc, char **argv)
{
    if (argc < 2) { printf("usage: dial <number>\n"); return 1; }
    const char *why = NULL;
    if (calls_dial(argv[1], &why) != ESP_OK) { printf("Refused: %s\n", why); return 1; }
    return 0;
}

static int cmd_answer(int argc, char **argv)
{
    const char *why = NULL;
    if (calls_answer(&why) != ESP_OK) { printf("Refused: %s\n", why); return 1; }
    return 0;
}

static int cmd_hangup(int argc, char **argv)
{
    const char *why = NULL;
    if (calls_hangup(&why) != ESP_OK) { printf("Refused: %s\n", why); return 1; }
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    if (argc < 3) { printf("usage: gain <input dB, 0..42> <output volume, 0..100>  (saved)\n"); return 1; }
    voice_set_gains(atof(argv[1]), atoi(argv[2]));
    settings_set("audio_in_db", argv[1]);
    settings_set("audio_out_vol", argv[2]);
    printf("Codec input %s dB, output %s, saved\n", argv[1], argv[2]);
    return 0;
}

static int cmd_peers(int argc, char **argv) { net_print_peers(); return 0; }
static int cmd_reboot(int argc, char **argv) { gw_restart(); return 0; }

// Drops microlink's cached peer table (NVS namespace "ml_peers") and restarts.
// The node key is kept, so the gateway rejoins the tailnet as the same device
// and gets a fresh peer list from the coordination server.
static int cmd_peers_clear(int argc, char **argv)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ml_peers", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("No cached peers\n");
        return 0;
    }
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("Failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Peer cache cleared, restarting\n");
    gw_restart();
    return 0;
}
static int cmd_setup(int argc, char **argv) { gw_restart_into_setup(); return 0; }

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = name, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "gw>";
    rc.task_stack_size = 8192;
    rc.task_core_id = 1;
    esp_console_dev_usb_serial_jtag_config_t uc = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&uc, &rc, &repl));
    esp_console_register_help_command();
    reg("status", "WiFi, tailnet and modem state", cmd_status);
    reg("config", "Show the saved settings", cmd_config);
    reg("set", "set <key> [value]: change a setting (no value = erase)", cmd_set);
    reg("at", "Send an AT command to the modem, e.g. 'at AT+CSQ'", cmd_at);
    reg("peers", "List tailnet peers", cmd_peers);
    reg("peers_clear", "Forget cached tailnet peers and restart (keeps the node key)", cmd_peers_clear);
    reg("sms", "sms <number> <text>: queue an SMS", cmd_sms);
    reg("inbox", "List received SMS kept in flash", cmd_inbox);
    reg("dial", "dial <number>: start a call", cmd_dial);
    reg("answer", "Answer the incoming call", cmd_answer);
    reg("hangup", "Hang up / reject", cmd_hangup);
    reg("gain", "gain <input dB> <output volume>: codec levels for calls (saved)", cmd_gain);
    reg("inbox_del", "inbox_del <id>: delete a received SMS", cmd_inbox_del);
    reg("reboot", "Shut the modem down cleanly and restart", cmd_reboot);
    reg("setup", "Restart into setup mode (setup WiFi + page)", cmd_setup);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
