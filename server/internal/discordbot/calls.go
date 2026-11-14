package discordbot

import (
	"bytes"
	"context"
	"fmt"
	"log/slog"
	"os"
	"time"

	"github.com/disgoorg/disgo/discord"

	"github.com/yyewolf/ltegtw/server/internal/gateway"
	"github.com/yyewolf/ltegtw/server/internal/recorder"
)

const (
	btnAnswer = "call:answer"
	btnReject = "call:reject"
	btnHangup = "call:hangup"

	// Discord's upload limit for bots without boosts.
	maxUpload = 10 << 20
)

// applyCall moves the call card and the voice session to the new state.
func (b *Bot) applyCall(c gateway.Call) {
	if c.State == "" {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	prev := b.call
	// "ring" events only carry the number; keep what call_state told us.
	if c.Number == "" && prev.State != "idle" {
		c.Number = prev.Number
	}
	if c.Direction == "" && prev.State != "idle" {
		c.Direction = prev.Direction
	}
	b.call = c

	if c.State == "idle" {
		if b.card != 0 {
			b.updateCardLocked(c)
		}
		b.card = 0
		b.stopVoiceLocked(context.Background(), true)
		return
	}

	switch {
	case b.card == 0:
		b.createCardLocked(c)
	case c.State != prev.State || c.Number != prev.Number:
		b.updateCardLocked(c)
	}

	// Outgoing calls join early so the ringback tone is heard.
	if c.State == "active" || c.Direction == "outgoing" {
		b.startVoiceLocked(c)
	}
}

func callTitle(c gateway.Call) string {
	if c.Direction == "outgoing" {
		return "Outgoing call"
	}
	return "Incoming call"
}

func callView(c gateway.Call) (discord.Embed, []discord.LayoutComponent) {
	e := discord.Embed{
		Author: &discord.EmbedAuthor{Name: "📞 " + c.Number},
		Title:  callTitle(c),
	}
	if c.Number == "" {
		e.Author.Name = "📞 unknown number"
	}
	now := time.Now()
	var buttons []discord.InteractiveComponent
	switch c.State {
	case "ringing":
		e.Description = "Ringing…"
		e.Color = colorRinging
		buttons = []discord.InteractiveComponent{
			discord.NewSuccessButton("Answer", btnAnswer),
			discord.NewDangerButton("Reject", btnReject),
		}
	case "dialing":
		e.Description = "Dialing…"
		e.Color = colorRinging
		buttons = []discord.InteractiveComponent{discord.NewDangerButton("Hang up", btnHangup)}
	case "alerting":
		e.Description = "Ringing on the other side…"
		e.Color = colorRinging
		buttons = []discord.InteractiveComponent{discord.NewDangerButton("Hang up", btnHangup)}
	case "active":
		e.Description = "In call"
		e.Color = colorActive
		buttons = []discord.InteractiveComponent{discord.NewDangerButton("Hang up", btnHangup)}
	case "idle":
		e.Color = colorEnded
		switch c.Reason {
		case "missed":
			e.Description = "Missed call"
			e.Color = colorMissed
		case "rejected":
			e.Description = "Rejected"
		case "busy":
			e.Description = "Busy"
			e.Color = colorMissed
		default:
			if c.Previous == "active" || c.Duration > 0 {
				e.Description = "Call ended after " + (time.Duration(c.Duration) * time.Second).String()
			} else if c.Direction == "outgoing" {
				e.Description = "No answer"
			} else {
				e.Description = "Call ended"
			}
		}
	default:
		e.Description = c.State
	}
	e.Timestamp = &now
	e.Footer = &discord.EmbedFooter{Text: "Reply to this message to send an SMS"}
	comps := []discord.LayoutComponent{}
	if len(buttons) > 0 {
		comps = append(comps, discord.NewActionRow(buttons...))
	}
	return e, comps
}

func (b *Bot) createCardLocked(c gateway.Call) {
	e, comps := callView(c)
	m := discord.MessageCreate{Embeds: []discord.Embed{e}, Components: comps, AllowedMentions: &discord.AllowedMentions{}}
	if c.State == "ringing" && c.Direction != "outgoing" && len(b.notify) > 0 {
		m.Content = mentions(b.notify)
		m.AllowedMentions = &discord.AllowedMentions{Users: b.notify}
	}
	if msg := b.send(m); msg != nil {
		b.card = msg.ID
		b.rememberLocked(msg.ID, c.Number)
	}
}

func (b *Bot) updateCardLocked(c gateway.Call) {
	e, comps := callView(c)
	embeds := []discord.Embed{e}
	if _, err := b.dc.Rest.UpdateMessage(b.channelID, b.card, discord.MessageUpdate{Embeds: &embeds, Components: &comps}); err != nil {
		b.log.Error("update call card", slog.Any("err", err))
	}
	b.rememberLocked(b.card, c.Number)
}

// ---- voice ----

func (b *Bot) startVoiceLocked(c gateway.Call) {
	if b.voiceID == 0 || b.vc != nil {
		return
	}
	bridge, err := NewBridge(b.relay, b.dc.ID(), b.isAllowed, b.log.With("component", "bridge"))
	if err != nil {
		b.log.Error("audio bridge", slog.Any("err", err))
		return
	}
	vs := &voiceSession{bridge: bridge, number: c.Number}
	if b.cfg.RecordingsDir != "" {
		dir := c.Direction
		if dir == "" {
			dir = "call"
		}
		if rec, err := recorder.Start(b.cfg.RecordingsDir, dir, c.Number, time.Now()); err != nil {
			b.log.Error("start recording", slog.Any("err", err))
		} else {
			vs.rec = rec
			bridge.SetRecorder(rec)
		}
	}
	b.relay.Reset()
	go bridge.Run()

	conn := b.dc.VoiceManager.CreateConn(b.guildID)
	vs.conn = conn
	b.vc = vs
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		if err := conn.Open(ctx, b.voiceID, false, false); err != nil {
			b.log.Error("join voice channel", slog.Any("err", err))
			return
		}
		b.mu.Lock()
		defer b.mu.Unlock()
		if b.vc != vs { // the call ended while joining
			return
		}
		conn.SetOpusFrameReceiver(bridge.Receiver())
		conn.SetOpusFrameProvider(bridge.Provider())
		b.log.Info("joined voice channel", slog.String("number", vs.number))
	}()
}

// stopVoiceLocked leaves the voice channel and finishes the recording. With
// post set, the recording is uploaded to the channel.
func (b *Bot) stopVoiceLocked(ctx context.Context, post bool) <-chan struct{} {
	done := make(chan struct{})
	vs := b.vc
	if vs == nil {
		close(done)
		return done
	}
	b.vc = nil
	vs.bridge.SetRecorder(nil)
	go func() {
		defer close(done)
		closeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
		defer cancel()
		vs.conn.Close(closeCtx)
		vs.bridge.Stop()
		b.log.Info("left voice channel",
			slog.Uint64("to_discord", vs.bridge.framesToDiscord.Load()),
			slog.Uint64("to_phone", vs.bridge.framesToPhone.Load()))
		if vs.rec == nil {
			return
		}
		dur, err := vs.rec.Close()
		if err != nil {
			b.log.Error("finish recording", slog.Any("err", err))
			return
		}
		if dur < time.Second {
			return
		}
		b.log.Info("recording saved", slog.String("path", vs.rec.Path), slog.Duration("duration", dur))
		if post {
			b.postRecording(vs, dur)
		}
	}()
	return done
}

func (b *Bot) postRecording(vs *voiceSession, dur time.Duration) {
	data, err := os.ReadFile(vs.rec.Path)
	if err != nil {
		return
	}
	msg := fmt.Sprintf("🎙️ Recording of the call with **%s** (%s)", vs.number, dur.Round(time.Second))
	if len(data) > maxUpload {
		b.send(discord.MessageCreate{Content: msg + ", too large to upload, saved on the server.", AllowedMentions: &discord.AllowedMentions{}})
		return
	}
	name := vs.rec.Path[len(b.cfg.RecordingsDir):]
	for len(name) > 0 && (name[0] == '/' || name[0] == '\\') {
		name = name[1:]
	}
	b.send(discord.MessageCreate{
		Content:         msg,
		Files:           []*discord.File{discord.NewFile(name, "", bytes.NewReader(data))},
		AllowedMentions: &discord.AllowedMentions{},
	})
}
