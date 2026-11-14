package discordbot

import (
	"io"
	"log/slog"
	"math"
	"sync"
	"testing"

	"github.com/disgoorg/audio/opus"
	"github.com/disgoorg/disgo/voice"
	"github.com/disgoorg/snowflake/v2"
)

type fakeSink struct {
	mu   sync.Mutex
	in   [][]int16
	sent [][]int16
}

func (f *fakeSink) Pop() []int16 {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.in) == 0 {
		return nil
	}
	p := f.in[0]
	f.in = f.in[1:]
	return p
}

func (f *fakeSink) Send(pcm []int16) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.sent = append(f.sent, pcm)
	return nil
}

func tone(freq float64, n, off int) []int16 {
	p := make([]int16, n)
	for i := range p {
		p[i] = int16(8000 * math.Sin(2*math.Pi*freq*float64(off+i)/sampleRate))
	}
	return p
}

func energy(p []int16) float64 {
	var e float64
	for _, v := range p {
		e += float64(v) * float64(v)
	}
	return math.Sqrt(e / float64(len(p)))
}

func newTestBridge(t *testing.T, sink AudioSink) *Bridge {
	t.Helper()
	b, err := NewBridge(sink, 1, nil, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		close(b.stop) // Run was never started
		close(b.done)
		b.Stop()
	})
	return b
}

// Phone audio comes out of the provider as Opus that decodes back to audio.
func TestBridgePhoneToDiscord(t *testing.T) {
	sink := &fakeSink{}
	for i := 0; i < 10; i++ {
		sink.in = append(sink.in, tone(440, frameSamples, i*frameSamples))
	}
	b := newTestBridge(t, sink)
	dec, _ := opus.NewDecoder(48000, 2) // what a Discord client does
	defer dec.Destroy()

	var last []int16
	for i := 0; i < 10; i++ {
		b.tick()
		f, err := b.Provider().ProvideOpusFrame()
		if err != nil || len(f) == 0 {
			t.Fatalf("frame %d: %v %d", i, err, len(f))
		}
		pcm := make([]int16, 960*2)
		n, err := dec.Decode(f, pcm, false)
		if err != nil || n != 960 {
			t.Fatalf("decode: %v %d", err, n)
		}
		last = pcm[:n*2]
	}
	if e := energy(last); e < 1000 {
		t.Fatalf("decoded audio too quiet: %.0f", e)
	}
	// Underrun: silence is still sent so speaking stays on.
	b.tick()
	if f, _ := b.Provider().ProvideOpusFrame(); len(f) == 0 {
		t.Fatal("no frame on underrun")
	}
	// Nothing queued: nil (disgo then sends its own silence).
	if f, _ := b.Provider().ProvideOpusFrame(); f != nil {
		t.Fatal("expected nil when queue is empty")
	}
}

// Two Discord speakers are decoded and mixed into the frame sent to the phone.
func TestBridgeDiscordToPhoneMix(t *testing.T) {
	sink := &fakeSink{}
	b := newTestBridge(t, sink)
	enc, _ := opus.NewEncoder(48000, 2, opus.ApplicationAudio)
	defer enc.Destroy()

	stereo := func(freq float64, off int) []int16 {
		m := make([]int16, 960)
		for i := range m {
			m[i] = int16(6000 * math.Sin(2*math.Pi*freq*float64(off+i)/48000))
		}
		s := make([]int16, 960*2)
		for i, v := range m {
			s[2*i], s[2*i+1] = v, v
		}
		return s
	}
	rx := b.Receiver()
	for i := 0; i < 8; i++ {
		for user, freq := range map[uint64]float64{10: 300, 11: 500} {
			buf := make([]byte, 1000)
			n, err := enc.Encode(stereo(freq, i*960), buf)
			if err != nil {
				t.Fatal(err)
			}
			if err := rx.ReceiveOpusFrame(snowflakeID(user), &voice.Packet{Opus: buf[:n]}); err != nil {
				t.Fatal(err)
			}
		}
		b.tick()
	}
	// The bot's own audio is ignored.
	if err := rx.ReceiveOpusFrame(1, &voice.Packet{Opus: []byte{0xff}}); err != nil {
		t.Fatal(err)
	}
	if len(b.users) != 2 {
		t.Fatalf("users %d", len(b.users))
	}
	if len(sink.sent) != 8 {
		t.Fatalf("sent %d", len(sink.sent))
	}
	got := sink.sent[len(sink.sent)-1]
	if len(got) != frameSamples || energy(got) < 1000 {
		t.Fatalf("mixed frame: len %d energy %.0f", len(got), energy(got))
	}
	rx.CleanupUser(10)
	if len(b.users) != 1 {
		t.Fatal("cleanup")
	}
	// Nobody talking: nil (the relay sends silence).
	b.mu.Lock()
	for _, s := range b.users {
		s.frames = nil
	}
	b.mu.Unlock()
	if b.mix() != nil {
		t.Fatal("expected nil mix")
	}
}

func TestMixClips(t *testing.T) {
	b := newTestBridge(t, &fakeSink{})
	loud := make([]int16, frameSamples)
	for i := range loud {
		loud[i] = 30000
	}
	b.users[5] = &speaker{dec: mustDec(t), frames: [][]int16{loud}}
	b.users[6] = &speaker{dec: mustDec(t), frames: [][]int16{loud}}
	if m := b.mix(); m[0] != 32767 {
		t.Fatalf("clip %d", m[0])
	}
}

func mustDec(t *testing.T) *opus.Decoder {
	d, err := opus.NewDecoder(sampleRate, 1)
	if err != nil {
		t.Fatal(err)
	}
	return d
}

func snowflakeID(v uint64) snowflake.ID { return snowflake.ID(v) }
