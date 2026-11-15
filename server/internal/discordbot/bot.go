// Package discordbot bridges the LTE gateway to a Discord channel: SMS in
// both directions, call control, and call audio in a voice channel.
package discordbot

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/disgoorg/disgo"
	"github.com/disgoorg/disgo/bot"
	"github.com/disgoorg/disgo/discord"
	"github.com/disgoorg/disgo/events"
	dgw "github.com/disgoorg/disgo/gateway"
	"github.com/disgoorg/disgo/voice"
	"github.com/disgoorg/godave/golibdave"
	"github.com/disgoorg/snowflake/v2"

	"github.com/yyewolf/ltegtw/server/internal/config"
	"github.com/yyewolf/ltegtw/server/internal/gateway"
	"github.com/yyewolf/ltegtw/server/internal/recorder"
	"github.com/yyewolf/ltegtw/server/internal/rtp"
)

const (
	emojiPending = "⏳"
	emojiSent    = "✅"
	emojiFailed  = "❌"

	colorSMS     = 0x3b82f6
	colorRinging = 0xf59e0b
	colorActive  = 0x22c55e
	colorEnded   = 0x6b7280
	colorMissed  = 0xef4444

	// Bot messages remembered for reply-to-send (number lookup).
	maxRemembered = 500
)

type Bot struct {
	cfg   *config.Config
	log   *slog.Logger
	gw    *gateway.Client
	relay *rtp.Relay
	dc    *bot.Client

	guildID   snowflake.ID
	channelID snowflake.ID
	voiceID   snowflake.ID
	allowed   map[snowflake.ID]bool
	notify    []snowflake.ID

	mu sync.Mutex
	// Message id → phone number, for replies to SMS embeds and call cards.
	numbers    map[snowflake.ID]string
	numberFIFO []snowflake.ID
	// Outbox id → the Discord message that asked for it.
	pending map[uint32]msgRef
	// Outbox results that arrived before the send request returned.
	early map[uint32]gateway.Event
	// Inbox ids already posted (the gateway may report an SMS twice: event
	// and inbox sync), with the time they were posted.
	posted map[uint32]time.Time

	call gateway.Call
	card snowflake.ID // message id of the current call card
	vc   *voiceSession
	// Closed when the last voice session has fully left the channel.
	lastStop <-chan struct{}
}

type msgRef struct {
	channel, message snowflake.ID
}

type voiceSession struct {
	conn   voice.Conn
	bridge *Bridge
	rec    *recorder.Recorder
	number string
}

func New(cfg *config.Config, gw *gateway.Client, relay *rtp.Relay, log *slog.Logger) (*Bot, error) {
	b := &Bot{
		cfg:       cfg,
		log:       log,
		gw:        gw,
		relay:     relay,
		guildID:   snowflake.MustParse(cfg.Discord.GuildID),
		channelID: snowflake.MustParse(cfg.Discord.ChannelID),
		allowed:   map[snowflake.ID]bool{},
		numbers:   map[snowflake.ID]string{},
		pending:   map[uint32]msgRef{},
		early:     map[uint32]gateway.Event{},
		posted:    map[uint32]time.Time{},
		call:      gateway.Call{State: "idle"},
	}
	if cfg.Discord.VoiceChannelID != "" {
		b.voiceID = snowflake.MustParse(cfg.Discord.VoiceChannelID)
	}
	for _, id := range cfg.Discord.AllowedUserIDs {
		b.allowed[snowflake.MustParse(id)] = true
	}
	for _, id := range cfg.Discord.NotifyUserIDs {
		b.notify = append(b.notify, snowflake.MustParse(id))
	}
	if len(b.allowed) == 0 {
		log.Warn("discord.allowed_user_ids is empty: every member of the guild can send SMS and place calls")
	}

	dc, err := disgo.New(cfg.Discord.Token,
		bot.WithLogger(log.With("component", "disgo")),
		bot.WithGatewayConfigOpts(dgw.WithIntents(
			dgw.IntentGuilds|dgw.IntentGuildMessages|dgw.IntentMessageContent|dgw.IntentGuildVoiceStates,
		)),
		bot.WithVoiceManagerConfigOpts(voice.WithDaveSessionCreateFunc(golibdave.NewSession)),
		bot.WithEventListenerFunc(b.onReady),
		bot.WithEventListenerFunc(b.onCommand),
		bot.WithEventListenerFunc(b.onComponent),
		bot.WithEventListenerFunc(b.onMessage),
	)
	if err != nil {
		return nil, err
	}
	b.dc = dc
	return b, nil
}

// Run connects to Discord and to the gateway's event stream, until ctx ends.
func (b *Bot) Run(ctx context.Context) error {
	if _, err := b.dc.Rest.SetGuildCommands(b.dc.ApplicationID, b.guildID, commands); err != nil {
		return fmt.Errorf("register commands: %w", err)
	}
	if err := b.dc.OpenGateway(ctx); err != nil {
		return fmt.Errorf("discord gateway: %w", err)
	}
	go b.gw.Listen(ctx, b.relay.LocalPort(), b.onEvent, b.log.With("component", "events"))
	go b.housekeeping(ctx)

	<-ctx.Done()
	closeCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	b.mu.Lock()
	done := b.stopVoiceLocked(closeCtx, false)
	b.mu.Unlock()
	<-done
	b.dc.Close(closeCtx)
	return nil
}

func (b *Bot) onReady(e *events.Ready) {
	b.log.Info("discord ready", slog.String("user", e.User.Username))
}

func (b *Bot) isAllowed(id snowflake.ID) bool {
	return len(b.allowed) == 0 || b.allowed[id]
}

// housekeeping forgets old posted ids and stale early results.
func (b *Bot) housekeeping(ctx context.Context) {
	t := time.NewTicker(10 * time.Minute)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			b.mu.Lock()
			for id, at := range b.posted {
				if now.Sub(at) > 24*time.Hour {
					delete(b.posted, id)
				}
			}
			b.early = map[uint32]gateway.Event{}
			b.mu.Unlock()
		}
	}
}

// remember records which phone number a bot message is about, so replying
// to it sends an SMS to that number. Caller holds b.mu.
func (b *Bot) rememberLocked(msg snowflake.ID, number string) {
	if number == "" || msg == 0 {
		return
	}
	if _, ok := b.numbers[msg]; !ok {
		b.numberFIFO = append(b.numberFIFO, msg)
	}
	b.numbers[msg] = number
	for len(b.numberFIFO) > maxRemembered {
		delete(b.numbers, b.numberFIFO[0])
		b.numberFIFO = b.numberFIFO[1:]
	}
}

// ---- gateway events ----

func (b *Bot) onEvent(e gateway.Event) {
	switch e.Type {
	case "hello":
		b.log.Info("gateway connected", slog.String("firmware", e.Firmware), slog.Int("sms_inbox", e.SMSInbox))
		if e.Call != nil {
			b.applyCall(*e.Call)
		}
		go b.syncInbox()
	case "disconnected":
		b.log.Warn("gateway event stream lost")
	case "sms_received":
		b.deliverSMS(gateway.SMS{ID: e.ID, From: e.From, Timestamp: e.Timestamp, Text: e.Text, Parts: e.Parts, Complete: e.Complete})
	case "sms_sent", "sms_failed":
		b.smsResult(e)
	case "ring":
		b.applyCall(gateway.Call{State: "ringing", Direction: "incoming", Number: e.Number})
	case "call_state":
		b.applyCall(gateway.Call{State: e.State, Direction: e.Direction, Number: e.Number, Duration: e.Duration, Previous: e.Previous, Reason: e.Reason})
	case "missed_call":
		// The call card already says "missed"; mention the notify list once more.
		if len(b.notify) > 0 {
			b.send(discord.MessageCreate{
				Content:         mentions(b.notify) + " missed call from **" + e.Number + "**",
				AllowedMentions: &discord.AllowedMentions{Users: b.notify},
			})
		}
	case "modem":
		b.log.Info("modem", slog.String("registration", e.Registration), slog.String("operator", e.Operator), slog.String("sim", e.SIM), slog.Int("rssi_dbm", e.RSSI))
	}
}

func (b *Bot) send(m discord.MessageCreate) *discord.Message {
	msg, err := b.dc.Rest.CreateMessage(b.channelID, m)
	if err != nil {
		b.log.Error("discord send", slog.Any("err", err))
		return nil
	}
	return msg
}

func mentions(ids []snowflake.ID) string {
	var s []string
	for _, id := range ids {
		s = append(s, "<@"+id.String()+">")
	}
	return strings.Join(s, " ")
}

// ---- SMS ----

// syncInbox posts every SMS still stored on the gateway, then deletes it there.
func (b *Bot) syncInbox() {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	var after uint32
	for {
		list, _, err := b.gw.Inbox(ctx, after, 20)
		if err != nil {
			b.log.Error("inbox sync", slog.Any("err", err))
			return
		}
		if len(list) == 0 {
			return
		}
		for _, m := range list {
			b.deliverSMS(m)
			after = max(after, m.ID)
		}
	}
}

func (b *Bot) deliverSMS(m gateway.SMS) {
	b.mu.Lock()
	_, done := b.posted[m.ID]
	b.mu.Unlock()
	if !done {
		msg := b.send(discord.MessageCreate{Embeds: []discord.Embed{smsEmbed(m)}})
		if msg == nil {
			return // keep it on the gateway, the next sync retries
		}
		b.mu.Lock()
		b.posted[m.ID] = time.Now()
		b.rememberLocked(msg.ID, m.From)
		b.mu.Unlock()
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := b.gw.DeleteSMS(ctx, m.ID); err != nil {
		b.log.Warn("delete sms on gateway", slog.Uint64("id", uint64(m.ID)), slog.Any("err", err))
	}
}

func smsEmbed(m gateway.SMS) discord.Embed {
	e := discord.Embed{
		Author:      &discord.EmbedAuthor{Name: "📱 " + m.From},
		Description: m.Text,
		Color:       colorSMS,
	}
	if e.Description == "" {
		e.Description = "*(empty message)*"
	}
	if t, err := time.Parse(time.RFC3339, m.Timestamp); err == nil {
		e.Timestamp = &t
	}
	footer := "Reply to this message to answer"
	if m.Parts > 1 {
		footer = fmt.Sprintf("%d parts · %s", m.Parts, footer)
	}
	if !m.Complete {
		footer = "⚠️ some parts never arrived · " + footer
	}
	e.Footer = &discord.EmbedFooter{Text: footer}
	return e
}

// numberFromEmbed recovers the phone number from a bot message after a
// restart, when it is no longer in memory.
func numberFromEmbed(m *discord.Message) string {
	if m == nil || len(m.Embeds) == 0 || m.Embeds[0].Author == nil {
		return ""
	}
	name := m.Embeds[0].Author.Name
	for _, p := range []string{"📱 ", "📞 "} {
		if strings.HasPrefix(name, p) {
			return strings.TrimPrefix(name, p)
		}
	}
	return ""
}

func (b *Bot) onMessage(e *events.GuildMessageCreate) {
	m := e.Message
	if m.Author.Bot || e.ChannelID != b.channelID || m.MessageReference == nil || m.MessageReference.MessageID == nil {
		return
	}
	b.mu.Lock()
	number := b.numbers[*m.MessageReference.MessageID]
	b.mu.Unlock()
	if number == "" && m.ReferencedMessage != nil && m.ReferencedMessage.Author.ID == b.dc.ID() {
		number = numberFromEmbed(m.ReferencedMessage)
	}
	if number == "" {
		return
	}
	if !b.isAllowed(m.Author.ID) {
		_ = b.dc.Rest.AddReaction(e.ChannelID, m.ID, "🚫")
		return
	}
	text := strings.TrimSpace(m.Content)
	if text == "" {
		return
	}
	b.sendSMS(number, text, msgRef{channel: e.ChannelID, message: m.ID})
}

// sendSMS queues an SMS on the gateway and tracks it with reactions on ref.
func (b *Bot) sendSMS(number, text string, ref msgRef) (*gateway.SendResult, error) {
	_ = b.dc.Rest.AddReaction(ref.channel, ref.message, emojiPending)
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	res, err := b.gw.SendSMS(ctx, number, text)
	if err != nil {
		b.log.Error("send sms", slog.String("to", number), slog.Any("err", err))
		_ = b.dc.Rest.RemoveOwnReaction(ref.channel, ref.message, emojiPending)
		_ = b.dc.Rest.AddReaction(ref.channel, ref.message, emojiFailed)
		_, _ = b.dc.Rest.CreateMessage(ref.channel, discord.MessageCreate{
			Content:          "Could not send: " + err.Error(),
			MessageReference: &discord.MessageReference{MessageID: &ref.message},
			AllowedMentions:  &discord.AllowedMentions{},
		})
		return nil, err
	}
	b.mu.Lock()
	b.rememberLocked(ref.message, number)
	early, ok := b.early[res.ID]
	if ok {
		delete(b.early, res.ID)
	} else {
		b.pending[res.ID] = ref
	}
	b.mu.Unlock()
	if ok {
		b.finishSMS(ref, early)
	}
	return res, nil
}

func (b *Bot) smsResult(e gateway.Event) {
	b.mu.Lock()
	ref, ok := b.pending[e.ID]
	if ok {
		delete(b.pending, e.ID)
	} else {
		b.early[e.ID] = e
	}
	b.mu.Unlock()
	if ok {
		b.finishSMS(ref, e)
	}
}

func (b *Bot) finishSMS(ref msgRef, e gateway.Event) {
	_ = b.dc.Rest.RemoveOwnReaction(ref.channel, ref.message, emojiPending)
	if e.Type == "sms_sent" {
		_ = b.dc.Rest.AddReaction(ref.channel, ref.message, emojiSent)
		return
	}
	_ = b.dc.Rest.AddReaction(ref.channel, ref.message, emojiFailed)
	reason := e.Error
	if reason == "" {
		reason = "unknown error"
	}
	_, _ = b.dc.Rest.CreateMessage(ref.channel, discord.MessageCreate{
		Content:          "SMS to " + e.To + " failed: " + reason,
		MessageReference: &discord.MessageReference{MessageID: &ref.message},
		AllowedMentions:  &discord.AllowedMentions{},
	})
}
