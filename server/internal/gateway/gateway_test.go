package gateway

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestClient(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer tok" {
			w.WriteHeader(http.StatusUnauthorized)
			w.Write([]byte(`{"error":"unauthorized"}`))
			return
		}
		switch r.Method + " " + r.URL.Path {
		case "POST /sms":
			var body map[string]string
			json.NewDecoder(r.Body).Decode(&body)
			if body["to"] != "+331" || body["text"] != "hi" {
				t.Errorf("body %v", body)
			}
			w.WriteHeader(http.StatusAccepted)
			w.Write([]byte(`{"id":7,"parts":1,"encoding":"gsm7","state":"queued"}`))
		case "GET /sms/inbox":
			if r.URL.RawQuery != "after=3&limit=20" {
				t.Errorf("query %s", r.URL.RawQuery)
			}
			w.Write([]byte(`{"messages":[{"id":4,"from":"+332","text":"yo","parts":1,"complete":true}],"total":1}`))
		case "POST /call/dial":
			w.WriteHeader(http.StatusConflict)
			w.Write([]byte(`{"error":"a call is already in progress"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer srv.Close()

	c, err := New(srv.URL+"/", "tok")
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	res, err := c.SendSMS(ctx, "+331", "hi")
	if err != nil || res.ID != 7 {
		t.Fatalf("send: %v %+v", err, res)
	}
	list, total, err := c.Inbox(ctx, 3, 20)
	if err != nil || total != 1 || len(list) != 1 || list[0].From != "+332" {
		t.Fatalf("inbox: %v %d %+v", err, total, list)
	}
	_, err = c.Dial(ctx, "+333")
	var api *APIError
	if !errors.As(err, &api) || api.Status != http.StatusConflict || api.Message != "a call is already in progress" {
		t.Fatalf("dial error: %v", err)
	}
	bad, _ := New(srv.URL, "")
	if _, err := bad.Status(ctx); !errors.As(err, &api) || api.Status != 401 {
		t.Fatalf("unauthorized: %v", err)
	}
	if _, err := New("ftp://x", ""); err == nil {
		t.Fatal("ftp accepted")
	}
}

func TestListen(t *testing.T) {
	up := websocket.Upgrader{}
	gotPort := make(chan int, 2)
	var conns atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/events" {
			w.WriteHeader(404)
			return
		}
		ws, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer ws.Close()
		n := conns.Add(1)
		var hello struct {
			Type string `json:"type"`
			Port int    `json:"port"`
		}
		if ws.ReadJSON(&hello) == nil && hello.Type == "audio" {
			gotPort <- hello.Port
		}
		ws.WriteMessage(websocket.TextMessage, []byte(`{"type":"hello","firmware":"0.3.0","call":{"state":"idle"}}`))
		ws.WriteMessage(websocket.TextMessage, []byte(`{"type":"ring","number":"+33612345678"}`))
		if n == 1 {
			return // drop the first connection to exercise the reconnect
		}
		time.Sleep(time.Second)
	}))
	defer srv.Close()

	c, _ := New(srv.URL, "")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	evs := make(chan Event, 16)
	go c.Listen(ctx, 4321, func(e Event) { evs <- e }, slog.New(slog.NewTextHandler(io.Discard, nil)))

	want := []string{"hello", "ring", "disconnected", "hello", "ring"}
	for i, w := range want {
		select {
		case e := <-evs:
			if e.Type != w {
				t.Fatalf("event %d: %s, want %s", i, e.Type, w)
			}
			if e.Type == "ring" && e.Number != "+33612345678" {
				t.Fatalf("number %q", e.Number)
			}
			if e.Type == "hello" && (e.Firmware != "0.3.0" || e.Call == nil || e.Call.State != "idle") {
				t.Fatalf("hello %+v", e)
			}
		case <-time.After(5 * time.Second):
			t.Fatalf("timeout waiting for %s", w)
		}
	}
	if p := <-gotPort; p != 4321 {
		t.Fatalf("audio port %d", p)
	}
}
