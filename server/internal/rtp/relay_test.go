package rtp

import (
	"io"
	"log/slog"
	"net"
	"testing"
	"time"
)

// A fake gateway sends PCMU frames; the relay buffers them and answers to the sender.
func TestRelayLoop(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	r, err := Listen("127.0.0.1:0", nil, func(ip net.IP) bool { return ip.IsLoopback() }, log)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	gw, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer gw.Close()
	dst := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: r.LocalPort()}
	for i := 0; i < 5; i++ {
		pkt := make([]byte, 12+160)
		pkt[0], pkt[1] = 0x80, 0
		pkt[2], pkt[3] = 0, byte(i)
		for k := 12; k < len(pkt); k++ {
			pkt[k] = 0x80 // loud positive µ-law
		}
		if _, err := gw.WriteToUDP(pkt, dst); err != nil {
			t.Fatal(err)
		}
	}
	deadline := time.Now().Add(2 * time.Second)
	for r.Jitter().Len() < 5 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	f := r.Pop()
	if f == nil || f[0] != 32124 {
		t.Fatalf("want decoded frame, got %v", f)
	}

	if err := r.Send(make([]int16, FrameSamples)); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 400)
	_ = gw.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := gw.ReadFromUDP(buf)
	if err != nil || n != 172 || buf[1] != 0x80 || buf[12] != 0xFF {
		t.Fatalf("uplink packet wrong: n=%d err=%v hdr=%x", n, err, buf[:13])
	}
}
