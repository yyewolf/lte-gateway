# LTE gateway firmware (rev0.6 board)

Done so far: WiFi + Tailscale (microlink), modem power-up and status, an HTTP API that only
answers on the tailnet, a setup WiFi for first configuration, a USB console (step 1), and SMS
in PDU mode with a flash-backed inbox and outbox (step 2). Calls, pushed events and voice come next.

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

## Console (USB-C J101)

    status              WiFi, tailnet and modem state
    config              saved settings (secrets masked)
    set <key> <value>   change a setting; then 'reboot'
    at <command>        raw AT command, e.g. at AT+CSQ
    peers               tailnet peers, direct or relayed
    sms <number> <text> queue an SMS
    inbox               received SMS kept in flash
    inbox_del <id>      delete one
    reboot              clean modem shutdown + restart
    setup               restart into setup mode

## Tests

The PDU encoder/decoder runs on a PC:

    cd test
    gcc -Wall -I../main test_pdu.c ../main/pdu.c -o test_pdu && ./test_pdu

## Notes

- microlink's own web config page is turned off (`CONFIG_ML_ENABLE_CONFIG_HTTPD=n`): it listens on every
  interface, LAN included, with no login, and shows the auth key.
- Core 0 is kept free for the audio path (step 4). The modem, API and console run on core 1;
  microlink puts its network I/O task on core 0 and the rest on core 1.
- The modem is restarted automatically if it stops answering AT three times in a row.
