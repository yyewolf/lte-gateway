// Package rtp relays call audio with the gateway: G.711 RTP over UDP, 8 kHz, 20 ms frames.
package rtp

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"

	"github.com/yyewolf/ltegtw/server/internal/g711"
)

// Relay owns the UDP socket. Downlink (the caller's voice) is jitter-buffered and pulled with
// Pop every 20 ms; uplink frames (what the caller hears) are sent with Send.
type Relay struct {
	conn   *net.UDPConn
	log    *slog.Logger
	jitter *Jitter

	mu      sync.Mutex
	gateway *net.UDPAddr // where uplink goes: learned from downlink, or the configured default
	allowIP func(net.IP) bool

	seq       uint16
	ts, ssrc  uint32
	marker    bool
	RxPackets atomic.Uint64
	TxPackets atomic.Uint64

	// OnFrame, if set, gets every downlink frame as it arrives (e.g. a recorder). Called from the reader.
	OnFrame func([]int16)
}

// Listen opens the UDP port. allowIP decides which sources are accepted (the gateway's tailnet IP).
func Listen(addr string, defaultGateway *net.UDPAddr, allowIP func(net.IP) bool, log *slog.Logger) (*Relay, error) {
	ua, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	c, err := net.ListenUDP("udp", ua)
	if err != nil {
		return nil, err
	}
	var b [6]byte
	_, _ = rand.Read(b[:])
	r := &Relay{
		conn: c, log: log, jitter: NewJitter(4, 12), // 80 ms prefill, 240 ms max
		gateway: defaultGateway, allowIP: allowIP,
		seq: binary.BigEndian.Uint16(b[0:]), ts: binary.BigEndian.Uint32(b[2:]), marker: true,
	}
	_, _ = rand.Read(b[:4])
	r.ssrc = binary.BigEndian.Uint32(b[:4])
	go r.read()
	return r, nil
}

// LocalPort is the bound UDP port.
func (r *Relay) LocalPort() int { return r.conn.LocalAddr().(*net.UDPAddr).Port }

// Close stops the relay.
func (r *Relay) Close() error { return r.conn.Close() }

// Reset clears the jitter buffer and starts a new uplink stream (new call).
func (r *Relay) Reset() {
	r.jitter.Reset()
	r.mu.Lock()
	r.marker = true
	r.mu.Unlock()
}

// Jitter exposes the downlink buffer counters.
func (r *Relay) Jitter() *Jitter { return r.jitter }

// Pop returns the next 20 ms of the caller's voice, or nil (silence / buffering).
func (r *Relay) Pop() []int16 { return r.jitter.Pop() }

func (r *Relay) read() {
	buf := make([]byte, 1500)
	for {
		n, src, err := r.conn.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			r.log.Warn("rtp read", "err", err)
			continue
		}
		if r.allowIP != nil && !r.allowIP(src.IP) {
			continue
		}
		seq, pcm, ok := parse(buf[:n])
		if !ok {
			continue
		}
		r.RxPackets.Add(1)
		r.mu.Lock()
		r.gateway = src // answer where the audio comes from
		r.mu.Unlock()
		if r.OnFrame != nil {
			r.OnFrame(pcm)
		}
		r.jitter.Push(seq, pcm)
	}
}

// parse decodes a PCMU/PCMA RTP packet into 160 samples (shorter payloads are zero-padded).
func parse(p []byte) (uint16, []int16, bool) {
	if len(p) < 12 || p[0]>>6 != 2 {
		return 0, nil, false
	}
	pt := p[1] & 0x7F
	if pt != 0 && pt != 8 {
		return 0, nil, false
	}
	off := 12 + 4*int(p[0]&0x0F)
	if p[0]&0x10 != 0 {
		if len(p) < off+4 {
			return 0, nil, false
		}
		off += 4 + 4*int(binary.BigEndian.Uint16(p[off+2:]))
	}
	end := len(p)
	if p[0]&0x20 != 0 && end > off {
		end -= int(p[end-1])
	}
	if end < off {
		return 0, nil, false
	}
	payload := p[off:end]
	if len(payload) > FrameSamples {
		payload = payload[:FrameSamples]
	}
	pcm := make([]int16, FrameSamples)
	for i, b := range payload {
		if pt == 0 {
			pcm[i] = g711.DecodeUlaw(b)
		} else {
			pcm[i] = g711.DecodeAlaw(b)
		}
	}
	return binary.BigEndian.Uint16(p[2:]), pcm, true
}

// Send sends 20 ms (160 samples) of audio to the gateway as PCMU. nil sends silence.
func (r *Relay) Send(pcm []int16) error {
	pkt := make([]byte, 12+FrameSamples)
	r.mu.Lock()
	dst := r.gateway
	pkt[0] = 0x80
	pkt[1] = 0
	if r.marker {
		pkt[1] |= 0x80
		r.marker = false
	}
	binary.BigEndian.PutUint16(pkt[2:], r.seq)
	binary.BigEndian.PutUint32(pkt[4:], r.ts)
	binary.BigEndian.PutUint32(pkt[8:], r.ssrc)
	r.seq++
	r.ts += FrameSamples
	r.mu.Unlock()
	if dst == nil {
		return errors.New("no gateway address yet")
	}
	if pcm == nil {
		for i := 12; i < len(pkt); i++ {
			pkt[i] = 0xFF // µ-law silence
		}
	} else {
		g711.EncodeUlawFrame(pkt[12:], pcm)
	}
	_, err := r.conn.WriteToUDP(pkt, dst)
	if err == nil {
		r.TxPackets.Add(1)
	}
	return err
}
