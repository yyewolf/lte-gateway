package gateway

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// Event is one message from ws://<gateway>/events. Fields not used by a type stay empty.
type Event struct {
	Type     string `json:"type"`
	UptimeMs int64  `json:"uptime_ms"`

	// hello
	Firmware string `json:"firmware"`
	SMSInbox int    `json:"sms_inbox"`
	Call     *Call  `json:"call"`

	// sms_received / sms_sent / sms_failed
	ID        uint32 `json:"id"`
	From      string `json:"from"`
	To        string `json:"to"`
	Timestamp string `json:"timestamp"`
	Text      string `json:"text"`
	Parts     int    `json:"parts"`
	Complete  bool   `json:"complete"`
	Error     string `json:"error"`

	// ring / call_state / missed_call
	Number    string `json:"number"`
	State     string `json:"state"`
	Direction string `json:"direction"`
	Previous  string `json:"previous"`
	Reason    string `json:"reason"`
	Duration  int    `json:"duration_s"`

	// modem
	Registration string `json:"registration"`
	Operator     string `json:"operator"`
	SIM          string `json:"sim"`
	RSSI         int    `json:"rssi_dbm"`
}

// Listen keeps a WebSocket to /events open until ctx ends, reconnecting with backoff.
// Every event goes to handle; onConnect runs after each (re)connect with the socket, so the
// caller can e.g. ask for audio on a given port. audioPort > 0 is sent as
// {"type":"audio","port":N} on every connect.
func (c *Client) Listen(ctx context.Context, audioPort int, handle func(Event), log *slog.Logger) {
	wsURL := "ws" + strings.TrimPrefix(c.base.String(), "http") + "/events"
	hdr := http.Header{}
	if c.token != "" {
		hdr.Set("Authorization", "Bearer "+c.token)
	}
	backoff := time.Second
	for ctx.Err() == nil {
		conn, _, err := websocket.DefaultDialer.DialContext(ctx, wsURL, hdr)
		if err != nil {
			log.Warn("events: connect failed", "url", wsURL, "err", err, "retry_in", backoff)
			select {
			case <-ctx.Done():
				return
			case <-time.After(backoff):
			}
			if backoff < 30*time.Second {
				backoff *= 2
			}
			continue
		}
		backoff = time.Second
		log.Info("events: connected", "url", wsURL)
		if audioPort > 0 {
			_ = conn.WriteJSON(map[string]any{"type": "audio", "port": audioPort})
		}
		c.readLoop(ctx, conn, handle, log)
		conn.Close()
		if ctx.Err() == nil {
			log.Warn("events: disconnected, reconnecting")
			handle(Event{Type: "disconnected"})
		}
	}
}

func (c *Client) readLoop(ctx context.Context, conn *websocket.Conn, handle func(Event), log *slog.Logger) {
	// The gateway pings every 20 s; no traffic for 60 s means the link is dead.
	const idle = 60 * time.Second
	_ = conn.SetReadDeadline(time.Now().Add(idle))
	conn.SetPingHandler(func(data string) error {
		_ = conn.SetReadDeadline(time.Now().Add(idle))
		return conn.WriteControl(websocket.PongMessage, []byte(data), time.Now().Add(5*time.Second))
	})
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			conn.Close()
		case <-done:
		}
	}()
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			if ctx.Err() == nil {
				log.Debug("events: read", "err", err)
			}
			return
		}
		_ = conn.SetReadDeadline(time.Now().Add(idle))
		var ev Event
		if err := json.Unmarshal(data, &ev); err != nil {
			log.Warn("events: bad message", "err", err)
			continue
		}
		handle(ev)
	}
}
