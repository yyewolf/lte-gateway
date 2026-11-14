// Package gateway talks to the LTE gateway board over the tailnet: REST commands and the
// /events WebSocket.
package gateway

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Client is a REST client for the gateway API.
type Client struct {
	base  *url.URL
	token string
	http  *http.Client
}

// New creates a client for e.g. "http://lte-gateway". token may be empty.
func New(baseURL, token string) (*Client, error) {
	u, err := url.Parse(strings.TrimRight(baseURL, "/"))
	if err != nil {
		return nil, err
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil, fmt.Errorf("gateway url must start with http://, got %q", baseURL)
	}
	return &Client{base: u, token: token, http: &http.Client{Timeout: 30 * time.Second}}, nil
}

// Host is the gateway host name or IP.
func (c *Client) Host() string { return c.base.Hostname() }

// APIError is a non-2xx answer from the gateway.
type APIError struct {
	Status  int
	Message string
}

func (e *APIError) Error() string { return fmt.Sprintf("gateway: %d %s", e.Status, e.Message) }

func (c *Client) do(ctx context.Context, method, path string, body, out any) error {
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return err
		}
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.base.String()+path, rd)
	if err != nil {
		return err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode/100 != 2 {
		var e struct {
			Error string `json:"error"`
		}
		_ = json.Unmarshal(data, &e)
		if e.Error == "" {
			e.Error = strings.TrimSpace(string(data))
		}
		return &APIError{Status: resp.StatusCode, Message: e.Error}
	}
	if out != nil && len(data) > 0 {
		return json.Unmarshal(data, out)
	}
	return nil
}

// SMS is a received message.
type SMS struct {
	ID        uint32 `json:"id"`
	From      string `json:"from"`
	Timestamp string `json:"timestamp"`
	Text      string `json:"text"`
	Parts     int    `json:"parts"`
	Complete  bool   `json:"complete"`
}

// SendResult is the answer to SendSMS.
type SendResult struct {
	ID       uint32 `json:"id"`
	Parts    int    `json:"parts"`
	Encoding string `json:"encoding"`
	State    string `json:"state"`
}

// SendSMS queues a message on the gateway.
func (c *Client) SendSMS(ctx context.Context, to, text string) (*SendResult, error) {
	var r SendResult
	err := c.do(ctx, http.MethodPost, "/sms", map[string]string{"to": to, "text": text}, &r)
	return &r, err
}

// Inbox lists received messages with id > after, oldest first.
func (c *Client) Inbox(ctx context.Context, after uint32, limit int) ([]SMS, int, error) {
	var r struct {
		Messages []SMS `json:"messages"`
		Total    int   `json:"total"`
	}
	err := c.do(ctx, http.MethodGet, fmt.Sprintf("/sms/inbox?after=%d&limit=%d", after, limit), nil, &r)
	return r.Messages, r.Total, err
}

// DeleteSMS acknowledges a received message: the gateway drops it from flash.
func (c *Client) DeleteSMS(ctx context.Context, id uint32) error {
	return c.do(ctx, http.MethodDelete, fmt.Sprintf("/sms/inbox/%d", id), nil, nil)
}

// Call is the call state.
type Call struct {
	State     string `json:"state"`
	Direction string `json:"direction,omitempty"`
	Number    string `json:"number,omitempty"`
	Duration  int    `json:"duration_s,omitempty"`
	Previous  string `json:"previous,omitempty"`
	Reason    string `json:"reason,omitempty"`
}

func (c *Client) Dial(ctx context.Context, number string) (*Call, error) {
	var r Call
	err := c.do(ctx, http.MethodPost, "/call/dial", map[string]string{"number": number}, &r)
	return &r, err
}

func (c *Client) Answer(ctx context.Context) (*Call, error) {
	var r Call
	err := c.do(ctx, http.MethodPost, "/call/answer", nil, &r)
	return &r, err
}

func (c *Client) Hangup(ctx context.Context) (*Call, error) {
	var r Call
	err := c.do(ctx, http.MethodPost, "/call/hangup", nil, &r)
	return &r, err
}

// Status is the raw /status document.
func (c *Client) Status(ctx context.Context) (map[string]any, error) {
	var r map[string]any
	err := c.do(ctx, http.MethodGet, "/status", nil, &r)
	return r, err
}
