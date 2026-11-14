# ltegtw

A self-hosted LTE SMS + voice gateway: an ESP32-S3 board with a SIMCom LTE Cat-1 modem
(VoLTE) that joins a Tailscale tailnet over WiFi. It sends and receives SMS and relays
calls as an RTP audio stream. All the heavy logic (Discord bridge, recording, SIP) runs on
a separate machine on the tailnet.

## Layout

| Folder | What |
|---|---|
| `firmware/` | ESP-IDF firmware for the gateway board |
| `hardware/` | KiCad project, BOM, fabrication files (to come) |
| `server/` | Go server: Discord bridge for SMS, calls and call audio ([README](server/README.md)) |
