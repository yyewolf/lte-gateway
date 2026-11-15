# ltegtw-server

The server half of the LTE gateway: it bridges the gateway to Discord.

- **SMS in:** every received SMS is posted to a text channel as an embed, then removed from the gateway's flash. Messages that arrived while the server was down are picked up when it reconnects.
- **SMS out:** reply to an SMS embed (or a call card) to answer that number, or use `/send`. The reply gets ⏳, then ✅ once the network accepted it or ❌ with the reason.
- **Calls:** an incoming call posts a card with **Answer** / **Reject** buttons (and mentions `notify_user_ids`). The card follows the call: ringing, in call, ended after 1m5s, missed. `/call`, `/answer`, `/hangup` do the same from slash commands.
- **Call audio:** during a call the bot joins the voice channel. The caller is heard in Discord, and everyone allowed in the channel is mixed and sent to the caller. Outgoing calls join while dialing, so you hear the ringback.
- **Recordings (optional):** each call is saved as a stereo WAV (left: phone, right: Discord) and posted in the channel when it fits Discord's upload limit.
- `/status` shows the gateway: firmware, uptime, tailnet, operator, signal, call state.

It talks to the gateway over the tailnet only: REST for commands, a WebSocket (`/events`) for events, and G.711 RTP on UDP for call audio.

```
phone ⇄ LTE ⇄ gateway ──tailnet── ltegtw-server ⇄ Discord
               REST + WebSocket events
               RTP G.711 µ-law, 8 kHz, 20 ms  ⇄  Opus (DAVE encrypted)
```

## Requirements

- The server's host must be on the same tailnet as the gateway, and the gateway must be able to reach the server's UDP port (4000 by default). The gateway sends call audio to the address the server connected from.
- A Discord bot with the **Message Content** intent enabled (Developer Portal → Bot). Invite it with the `bot` and `applications.commands` scopes and these permissions on the channels: View Channel, Send Messages, Embed Links, Attach Files, Add Reactions, Read Message History, Connect, Speak.
- Go 1.24+, libopus and libdave (Discord's voice end-to-end encryption, DAVE) to build. The prebuilt libdave needs glibc 2.38+ (Debian 13, Ubuntu 24.04 or newer). The Docker image includes both.

## Configuration

Copy [`config.example.yaml`](config.example.yaml) to `config.yaml` and fill it in. Every key also has an environment variable that wins over the file:

| Key | Environment | |
|---|---|---|
| `gateway.url` | `LTEGTW_GATEWAY_URL` | e.g. `http://lte-gateway` or `http://100.64.125.24` |
| `gateway.token` | `LTEGTW_GATEWAY_TOKEN` | only if `api_token` is set on the gateway |
| `rtp.listen` | `LTEGTW_RTP_LISTEN` | default `:4000` |
| `discord.token` | `LTEGTW_DISCORD_TOKEN` | |
| `discord.guild_id` | `LTEGTW_DISCORD_GUILD_ID` | slash commands are registered on this guild |
| `discord.channel_id` | `LTEGTW_DISCORD_CHANNEL_ID` | text channel |
| `discord.voice_channel_id` | `LTEGTW_DISCORD_VOICE_CHANNEL_ID` | empty: calls without audio |
| `discord.allowed_user_ids` | `LTEGTW_DISCORD_ALLOWED_USER_IDS` | comma separated; empty allows everyone |
| `discord.notify_user_ids` | `LTEGTW_DISCORD_NOTIFY_USER_IDS` | mentioned on incoming and missed calls |
| `recordings_dir` | `LTEGTW_RECORDINGS_DIR` | empty disables recording |
| `log_level` | `LTEGTW_LOG_LEVEL` | `debug`, `info`, `warn`, `error` |

`allowed_user_ids` also decides whose voice reaches the caller: other people in the voice channel are not mixed in.

## Run with Docker

```sh
cd deploy
cp ../config.example.yaml config.yaml   # edit it
docker compose up -d --build
```

The compose file uses host networking, so run it on a machine that is already on the tailnet.

## Build and run locally

```sh
# libopus
sudo apt install libopus-dev pkg-config        # Debian/Ubuntu
# libdave v1.1.0, installed to ~/.local
go mod download
sh "$(go env GOMODCACHE)/github.com/disgoorg/godave@v0.3.0/scripts/libdave_install.sh" v1.1.0

export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
go build -o ltegtw-server ./cmd/ltegtw-server
./ltegtw-server -config config.yaml
```

A systemd unit is in [`deploy/ltegtw-server.service`](deploy/ltegtw-server.service) (config in `/etc/ltegtw/config.yaml`; use `recordings_dir: /var/lib/ltegtw/recordings`).

## Tests

```sh
PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig go test -race ./...
```

They cover G.711, the jitter buffer and RTP relay, the gateway REST client and event stream (against a fake gateway), the Opus bridge (encode, decode, mixing, clipping), the call card and SMS embeds, config loading, and the WAV recorder.

## Layout

| Path | |
|---|---|
| `cmd/ltegtw-server` | entry point |
| `internal/config` | YAML + environment |
| `internal/gateway` | gateway REST client and WebSocket events |
| `internal/rtp` | UDP relay, jitter buffer |
| `internal/g711` | µ-law / A-law |
| `internal/discordbot` | Discord: SMS, slash commands, call cards, voice bridge |
| `internal/recorder` | WAV recordings |
| `deploy/` | docker compose, systemd unit |

## How call audio flows

One 20 ms clock drives both directions. Each tick pops one frame from the RTP jitter buffer (the caller), encodes it as 8 kHz mono Opus for Discord, and mixes one decoded frame from each Discord speaker to send back as one RTP packet. Opus works at 8 kHz natively, so there is no resampling. When the jitter buffer runs dry the tick sends silence instead, so Discord keeps showing the caller as speaking.
