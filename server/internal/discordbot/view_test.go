package discordbot

import (
	"strings"
	"testing"

	"github.com/disgoorg/disgo/discord"

	"github.com/yyewolf/ltegtw/server/internal/gateway"
)

func TestCallView(t *testing.T) {
	cases := []struct {
		call    gateway.Call
		desc    string
		buttons int
	}{
		{gateway.Call{State: "ringing", Direction: "incoming", Number: "+33612345678"}, "Ringing", 2},
		{gateway.Call{State: "dialing", Direction: "outgoing", Number: "+33612345678"}, "Dialing", 1},
		{gateway.Call{State: "active", Direction: "incoming", Number: "+33612345678"}, "In call", 1},
		{gateway.Call{State: "idle", Direction: "incoming", Reason: "missed"}, "Missed", 0},
		{gateway.Call{State: "idle", Direction: "incoming", Previous: "active", Duration: 65}, "1m5s", 0},
		{gateway.Call{State: "idle", Direction: "outgoing", Previous: "alerting", Reason: "hangup"}, "No answer", 0},
	}
	for _, c := range cases {
		e, comps := callView(c.call)
		if !strings.Contains(e.Description, c.desc) {
			t.Errorf("%+v: description %q", c.call, e.Description)
		}
		n := 0
		for _, row := range comps {
			n += len(row.(discord.ActionRowComponent).Components)
		}
		if n != c.buttons {
			t.Errorf("%+v: %d buttons", c.call, n)
		}
		if numberFromEmbed(&discord.Message{Embeds: []discord.Embed{e}}) != strings.TrimPrefix(e.Author.Name, "📞 ") {
			t.Errorf("number not recoverable from card")
		}
	}
}

func TestSMSEmbed(t *testing.T) {
	e := smsEmbed(gateway.SMS{ID: 3, From: "+33611111111", Timestamp: "2026-11-14T10:20:30+01:00", Text: "hi", Parts: 2, Complete: false})
	if e.Timestamp == nil || e.Timestamp.UTC().Hour() != 9 {
		t.Fatalf("timestamp %v", e.Timestamp)
	}
	if !strings.Contains(e.Footer.Text, "2 parts") || !strings.Contains(e.Footer.Text, "never arrived") {
		t.Fatalf("footer %q", e.Footer.Text)
	}
	if got := numberFromEmbed(&discord.Message{Embeds: []discord.Embed{e}}); got != "+33611111111" {
		t.Fatalf("number %q", got)
	}
	if numberFromEmbed(&discord.Message{}) != "" {
		t.Fatal("empty message")
	}
}

func TestStatusEmbed(t *testing.T) {
	e := statusEmbed(map[string]any{
		"firmware": "0.3.0", "uptime_s": 125.0,
		"modem":   map[string]any{"operator": "Free", "rssi_dbm": -71.0, "registration": "home", "sim": "READY"},
		"tailnet": map[string]any{"state": "connected", "ip": "100.64.125.24"},
		"call":    map[string]any{"state": "idle"},
	})
	want := map[string]string{"Firmware": "0.3.0", "Uptime": "2m5s", "Signal": "-71 dBm", "Operator": "Free", "Call": "idle", "Unread SMS on gateway": "—"}
	for _, f := range e.Fields {
		if w, ok := want[f.Name]; ok && f.Value != w {
			t.Errorf("%s = %q, want %q", f.Name, f.Value, w)
		}
	}
}
