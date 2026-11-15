# lte-gateway

A self-hosted LTE SMS + voice gateway: an ESP32-S3 board with a SIMCom LTE Cat-1 modem
(VoLTE) that joins a Tailscale tailnet over WiFi. It sends and receives SMS and relays
calls as an RTP audio stream. All the heavy logic (Discord bridge, recording, SIP) runs on
a separate machine on the tailnet.

## Layout

| Folder | What |
|---|---|
| `firmware/` | ESP-IDF firmware for the gateway board |
| `hardware/` | KiCad project for the board and its fabrication files ([README](hardware/README.md)) |
| `server/` | Go server: Discord bridge for SMS, calls and call audio ([README](server/README.md)) |
| `case/` | 3D-printable snap-fit enclosure ([README](case/README.md)) |

## License

The firmware, server, hardware and case here are BSD 3-Clause — see [LICENSE](LICENSE).

Tailscale compatibility does not come from Tailscale: `firmware/components/microlink` is
vendored from [CamM2325/microlink](https://github.com/CamM2325/microlink) (MIT, upstream
commit 216da33), a reverse-engineered implementation of the Tailscale protocol for embedded
devices, unaffiliated with and not endorsed by Tailscale Inc. The vendored copy carries
local patches (listed in
[firmware/components/VENDORED.txt](firmware/components/VENDORED.txt)), so it is committed
here rather than fetched at build time. `firmware/components/wireguard_lwip` and the x25519
sources keep their own licenses, included next to the code.
