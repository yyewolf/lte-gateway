# LTE gateway firmware (rev0.6 board)

WiFi + Tailscale (microlink), modem power-up and status, an HTTP API that only answers on the
tailnet, a setup WiFi for first configuration, a USB console, SMS in PDU mode with a
flash-backed inbox and outbox, call control, events pushed over a WebSocket, and call audio
relayed as G.711 RTP over the tailnet.

Builds on ESP-IDF 6.1 and 5.4 (both checked). On 6.x, cJSON is fetched from the component registry on the first build. microlink is vendored in `components/`
(commit 216da33, MIT, with small ESP-IDF 6 patches listed in `components/VENDORED.txt`).

## Build and flash

From an "ESP-IDF CMD" or PowerShell window:

    cd firmware
    idf.py set-target esp32s3
    idf.py build
    idf.py -p COM5 erase-flash flash monitor

`erase-flash` is only needed the first time (new partition table). Later just `flash monitor`.
If flashing can't connect: hold BOOT (SW202), tap RESET (SW201), release BOOT.
Leave the monitor with Ctrl+].

## First setup

1. In the Tailscale admin console, create an auth key (Settings > Keys > Generate auth key).
   A one-off, non-ephemeral key is fine. After the first login the board keeps its own keys.
2. Power the board. With no WiFi saved it starts in **setup mode**: the red LED blinks and
   a WiFi called `LTE-Gateway-setup-XXXX` appears.
3. Join it from a phone or laptop. The setup page should open by itself; otherwise open
   http://192.168.4.1/.
4. Fill in the WiFi network and password, the auth key, a device name (default `lte-gateway`)
   and optionally your server's tailnet IP and an API token. Save: the board restarts.
5. In the Tailscale admin console, open the new machine and choose **Disable key expiry**,
   otherwise it drops off the tailnet after 180 days.

To get back into setup mode later: hold BOOT for 5 s while the board runs, or type `setup`
on the console. Setup mode goes back to normal after 15 minutes without a save.
The setup page exists only in setup mode, so nothing on your LAN can reach it later.

## LEDs

- Red: blinking = setup mode, on = not on the tailnet yet, off = on the tailnet.
- Green: on = modem registered on the LTE network.

## API (tailnet only)

    curl http://lte-gateway/status          # MagicDNS name, or the 100.x.y.z IP

Requests are refused (403) unless they arrive on the board's tailnet IP from a 100.64.0.0/10
address. If `api_token` is set, add `-H "Authorization: Bearer <token>"`.

## SMS API

Send (UTF-8; GSM-7 when possible, UCS-2 otherwise; long texts split into up to 10 parts):

    curl -X POST http://lte-gateway/sms -H "Content-Type: application/json" \
         -d '{"to": "+33612345678", "text": "Hello from the gateway"}'
    -> 202 {"id": 1, "parts": 1, "encoding": "gsm7", "state": "queued"}

    curl http://lte-gateway/sms/outbox/1
    -> {"id": 1, "to": "+33612345678", "state": "sent", "parts": 1, "sent_parts": 1, "attempts": 1}

The outbox lives in flash: a message queued while the modem is down goes out when it is back.
Each message gets 3 attempts (30 s apart), then `"state": "failed"` with the modem's error.

Receive: incoming SMS are read off the modem, long ones are reassembled (an incomplete one is
delivered after 1 hour with `"complete": false`), and kept in flash until you delete them:

    curl "http://lte-gateway/sms/inbox?after=0&limit=20"     # oldest first
    -> {"messages": [{"id": 3, "from": "+33698765432", "timestamp": "2026-11-14T17:02:11+01:00",
                      "text": "Hi!", "parts": 1, "complete": true}], "total": 1}
    curl -X DELETE http://lte-gateway/sms/inbox/3           # after your server stored it

Your server should store each message, then DELETE it. Step 3 adds a push notification so it
doesn't have to poll.

## Calls

    curl http://lte-gateway/call                                   # {"state": "idle"}
    curl -X POST http://lte-gateway/call/dial -d '{"number": "+33612345678"}'
    curl -X POST http://lte-gateway/call/answer
    curl -X POST http://lte-gateway/call/hangup                    # also rejects a ringing call

States: `idle`, `ringing` (incoming), `dialing`, `alerting` (outgoing, the other side rings), `active`.

## Events (WebSocket)

Connect to `ws://lte-gateway/events` from a tailnet machine (same checks and token as the API).
Every message is one JSON object with a `type` and `uptime_ms`:

| type | fields |
|---|---|
| `hello` | `firmware`, `sms_inbox` (count waiting), `call` (current state) — sent on connect |
| `sms_received` | `id`, `from`, `timestamp`, `text`, `parts`, `complete` — still DELETE it once stored |
| `sms_sent` / `sms_failed` | `id`, `to`, `parts`, `error` |
| `ring` | `number` (caller ID, "" if withheld) — about every 5 s while ringing |
| `call_state` | `state`, `direction`, `number`; when it ends: `previous`, `reason` (`ended`, `hangup`, `rejected`, `missed`, `busy`, `no_answer`, `failed`, `modem_down`), `duration_s` |
| `missed_call` | `number` |
| `modem` | `registration`, `operator`, `sim`, `rssi_dbm` — on changes |

The board sends a WebSocket ping every 20 s. Up to 4 clients at once.

## Call audio (RTP)

While a call is `active`, the board sends the caller's voice as RTP (G.711 µ-law, payload type 0,
8 kHz, 160 samples = 20 ms per packet) over the tailnet, and plays the RTP it receives back to
the caller (PCMU or PCMA). The board listens on UDP port 4000 and sends to the newest
`/events` client, port 4000 by default. A client can pick another port by sending
`{"type": "audio", "port": 5004}` on the WebSocket. Only packets from that client's IP are
played. The board keeps a 60–160 ms jitter buffer; `/status` shows packet counters under `voice`.

Codec levels: `gain <input dB> <output volume>` on the console (saved; default 18 dB / 70).

## Console (USB-C J101)

    status              WiFi, tailnet and modem state
    config              saved settings (secrets masked)
    set <key> <value>   change a setting; then 'reboot'
    at <command>        raw AT command, e.g. at AT+CSQ
    peers               tailnet peers, direct or relayed
    sms <number> <text> queue an SMS
    inbox               received SMS kept in flash
    inbox_del <id>      delete one
    dial <number>       start a call; answer, hangup
    gain <in dB> <vol>  codec levels for calls (saved)
    reboot              clean modem shutdown + restart
    setup               restart into setup mode

## Tests

The PDU encoder/decoder runs on a PC:

    cd test
    gcc -Wall -I../main test_pdu.c ../main/pdu.c -o test_pdu && ./test_pdu
    gcc -Wall -I../main test_g711.c ../main/g711.c -o test_g711 && ./test_g711

## Notes

- microlink's own web config page is turned off (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`): it listens on every
  interface, LAN included, with no login, and shows the auth key.
- The audio loop runs on core 0 at high priority, paced by the I2S clock. The modem, API,
  events and console run on core 1; microlink puts its network I/O task on core 0 and the
  rest on core 1.
- The modem is restarted automatically if it stops answering AT three times in a row.
- The gateway keeps at most 16 tailnet peers (`CONFIG_ML_MAX_PEERS`). Extra peers are dropped
  (`Peer table full` in the log) and cannot reach it. On a bigger tailnet, limit what the gateway
  sees with a Tailscale ACL (it only gets the peers allowed to talk to it), and set
  `priority_peer` to the server's tailnet IP so the server always gets a slot.
- Peers that can't reach the gateway directly relay through its home DERP region,
  `CONFIG_ML_DERP_REGION` (4, Frankfurt, by default here; microlink's own default is Dallas).
