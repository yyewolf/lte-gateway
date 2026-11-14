package discordbot

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/disgoorg/disgo/discord"
	"github.com/disgoorg/disgo/events"

	"github.com/yyewolf/ltegtw/server/internal/gateway"
)

var commands = []discord.ApplicationCommandCreate{
	discord.SlashCommandCreate{
		Name:        "send",
		Description: "Send an SMS",
		Options: []discord.ApplicationCommandOption{
			discord.ApplicationCommandOptionString{Name: "number", Description: "Phone number, e.g. +33612345678", Required: true},
			discord.ApplicationCommandOptionString{Name: "message", Description: "Text to send", Required: true},
		},
	},
	discord.SlashCommandCreate{
		Name:        "call",
		Description: "Call a phone number; audio goes through the voice channel",
		Options: []discord.ApplicationCommandOption{
			discord.ApplicationCommandOptionString{Name: "number", Description: "Phone number, e.g. +33612345678", Required: true},
		},
	},
	discord.SlashCommandCreate{Name: "answer", Description: "Answer the incoming call"},
	discord.SlashCommandCreate{Name: "hangup", Description: "Hang up or reject the current call"},
	discord.SlashCommandCreate{Name: "status", Description: "Show the gateway status"},
}

func ephemeral(text string) discord.MessageCreate {
	return discord.MessageCreate{Content: text, Flags: discord.MessageFlagEphemeral, AllowedMentions: &discord.AllowedMentions{}}
}

func errText(err error) string {
	var api *gateway.APIError
	if errors.As(err, &api) {
		return api.Message
	}
	return err.Error()
}

func (b *Bot) onCommand(e *events.ApplicationCommandInteractionCreate) {
	data := e.SlashCommandInteractionData()
	if !b.isAllowed(e.User().ID) {
		_ = e.CreateMessage(ephemeral("🚫 You are not allowed to use the gateway."))
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	switch data.CommandName() {
	case "send":
		number, text := strings.TrimSpace(data.String("number")), data.String("message")
		if err := e.CreateMessage(discord.MessageCreate{
			Content:         fmt.Sprintf("📤 **%s**: %s", number, text),
			AllowedMentions: &discord.AllowedMentions{},
		}); err != nil {
			b.log.Error("interaction reply", slog.Any("err", err))
			return
		}
		msg, err := b.dc.Rest.GetInteractionResponse(e.ApplicationID(), e.Token())
		if err != nil {
			b.log.Error("interaction response", slog.Any("err", err))
			return
		}
		b.sendSMS(number, text, msgRef{channel: msg.ChannelID, message: msg.ID})

	case "call":
		number := strings.TrimSpace(data.String("number"))
		if _, err := b.gw.Dial(ctx, number); err != nil {
			_ = e.CreateMessage(ephemeral("Could not call: " + errText(err)))
			return
		}
		note := ""
		if b.voiceID != 0 {
			note = " Join <#" + b.voiceID.String() + "> to talk."
		}
		_ = e.CreateMessage(ephemeral("📞 Calling " + number + "…" + note))

	case "answer":
		if _, err := b.gw.Answer(ctx); err != nil {
			_ = e.CreateMessage(ephemeral("Could not answer: " + errText(err)))
			return
		}
		_ = e.CreateMessage(ephemeral("Answered."))

	case "hangup":
		if _, err := b.gw.Hangup(ctx); err != nil {
			_ = e.CreateMessage(ephemeral("Could not hang up: " + errText(err)))
			return
		}
		_ = e.CreateMessage(ephemeral("Hung up."))

	case "status":
		st, err := b.gw.Status(ctx)
		if err != nil {
			_ = e.CreateMessage(ephemeral("Gateway unreachable: " + errText(err)))
			return
		}
		_ = e.CreateMessage(discord.MessageCreate{Embeds: []discord.Embed{statusEmbed(st)}, Flags: discord.MessageFlagEphemeral})
	}
}

func (b *Bot) onComponent(e *events.ComponentInteractionCreate) {
	id := e.Data.CustomID()
	if !strings.HasPrefix(id, "call:") {
		return
	}
	if !b.isAllowed(e.User().ID) {
		_ = e.CreateMessage(ephemeral("🚫 You are not allowed to use the gateway."))
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	var err error
	switch id {
	case btnAnswer:
		_, err = b.gw.Answer(ctx)
	case btnReject, btnHangup:
		_, err = b.gw.Hangup(ctx)
	}
	if err != nil {
		_ = e.CreateMessage(ephemeral("Gateway: " + errText(err)))
		return
	}
	// The card is updated by the call_state event that follows.
	_ = e.DeferUpdateMessage()
}

// statusEmbed renders the gateway's /status JSON.
func statusEmbed(st map[string]any) discord.Embed {
	get := func(path ...string) any {
		var cur any = st
		for _, p := range path {
			m, ok := cur.(map[string]any)
			if !ok {
				return nil
			}
			cur = m[p]
		}
		return cur
	}
	str := func(path ...string) string {
		v := get(path...)
		switch v := v.(type) {
		case nil:
			return "—"
		case string:
			if v == "" {
				return "—"
			}
			return v
		case float64:
			return fmt.Sprintf("%g", v)
		case bool:
			if v {
				return "yes"
			}
			return "no"
		}
		return fmt.Sprint(v)
	}
	uptime := "—"
	if v, ok := get("uptime_s").(float64); ok {
		uptime = (time.Duration(v) * time.Second).String()
	}
	signal := str("modem", "rssi_dbm")
	if signal != "—" {
		signal += " dBm"
	}
	call := str("call", "state")
	if n := str("call", "number"); n != "—" {
		call += " · " + n
	}
	return discord.Embed{
		Title: "LTE gateway",
		Color: colorSMS,
		Fields: []discord.EmbedField{
			{Name: "Firmware", Value: str("firmware"), Inline: ptr(true)},
			{Name: "Uptime", Value: uptime, Inline: ptr(true)},
			{Name: "Tailnet", Value: str("tailnet", "state") + " · " + str("tailnet", "ip"), Inline: ptr(true)},
			{Name: "Operator", Value: str("modem", "operator"), Inline: ptr(true)},
			{Name: "Registration", Value: str("modem", "registration"), Inline: ptr(true)},
			{Name: "Signal", Value: signal, Inline: ptr(true)},
			{Name: "SIM", Value: str("modem", "sim"), Inline: ptr(true)},
			{Name: "WiFi", Value: str("wifi", "ssid") + " · " + str("wifi", "rssi_dbm") + " dBm", Inline: ptr(true)},
			{Name: "Call", Value: call, Inline: ptr(true)},
			{Name: "Unread SMS on gateway", Value: str("sms_inbox"), Inline: ptr(true)},
		},
	}
}

func ptr[T any](v T) *T { return &v }
