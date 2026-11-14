package discordbot

import (
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"github.com/disgoorg/audio/opus"
	"github.com/disgoorg/disgo/voice"
	"github.com/disgoorg/snowflake/v2"

	"github.com/yyewolf/ltegtw/server/internal/recorder"
	"github.com/yyewolf/ltegtw/server/internal/rtp"
)

// Discord audio is Opus; the phone side is 8 kHz mono PCM. Opus encodes and
// decodes at 8 kHz natively, so no resampling is needed: a 20 ms frame is
// 160 samples on both sides.
const (
	sampleRate   = 8000
	frameSamples = rtp.FrameSamples
	// Max decoded frame per user kept for mixing (about 100 ms).
	userQueue = 5
	// Encoded phone frames waiting for the Discord sender.
	sendQueue = 4
)

// AudioSink is where the bridge sends Discord audio towards the phone.
type AudioSink interface {
	Pop() []int16
	Send(pcm []int16) error
}

// Bridge connects the RTP relay to a Discord voice connection. One 20 ms
// clock drives both directions: each tick pops one phone frame (encoded for
// Discord) and mixes one frame from each Discord speaker (sent to the phone).
type Bridge struct {
	log   *slog.Logger
	sink  AudioSink
	self  snowflake.ID
	allow func(snowflake.ID) bool

	enc  *opus.Encoder
	out  chan []byte
	stop chan struct{}
	done chan struct{}

	mu    sync.Mutex
	users map[snowflake.ID]*speaker

	rec atomic.Pointer[recorder.Recorder]

	// stats
	framesToDiscord atomic.Uint64
	framesToPhone   atomic.Uint64
}

type speaker struct {
	dec    *opus.Decoder
	frames [][]int16
}

func NewBridge(sink AudioSink, self snowflake.ID, allow func(snowflake.ID) bool, log *slog.Logger) (*Bridge, error) {
	enc, err := opus.NewEncoder(sampleRate, 1, opus.ApplicationVoip)
	if err != nil {
		return nil, err
	}
	_ = enc.Ctl(opus.SetBitrate(24000))
	return &Bridge{
		log:   log,
		sink:  sink,
		self:  self,
		allow: allow,
		enc:   enc,
		out:   make(chan []byte, sendQueue),
		stop:  make(chan struct{}),
		done:  make(chan struct{}),
		users: map[snowflake.ID]*speaker{},
	}, nil
}

// SetRecorder starts (or with nil stops) writing both sides to rec.
func (b *Bridge) SetRecorder(rec *recorder.Recorder) { b.rec.Store(rec) }

// Run ticks every 20 ms until Stop is called.
func (b *Bridge) Run() {
	defer close(b.done)
	t := time.NewTicker(20 * time.Millisecond)
	defer t.Stop()
	for {
		select {
		case <-b.stop:
			return
		case <-t.C:
			b.tick()
		}
	}
}

func (b *Bridge) tick() {
	phone := b.sink.Pop()
	pcm := phone
	if pcm == nil {
		pcm = silence
	}
	buf := make([]byte, 400)
	if n, err := b.enc.Encode(pcm, buf); err != nil {
		b.log.Debug("opus encode", slog.Any("err", err))
	} else {
		b.enqueue(buf[:n])
	}

	mix := b.mix()
	if err := b.sink.Send(mix); err != nil {
		b.log.Debug("rtp send", slog.Any("err", err))
	} else {
		b.framesToPhone.Add(1)
	}
	if rec := b.rec.Load(); rec != nil {
		rec.Write(orSilence(phone), orSilence(mix))
	}
}

var silence = make([]int16, frameSamples)

func orSilence(p []int16) []int16 {
	if p == nil {
		return silence
	}
	return p
}

// enqueue drops the oldest frame when the sender is not keeping up (for
// example before the DAVE session is ready).
func (b *Bridge) enqueue(frame []byte) {
	for {
		select {
		case b.out <- frame:
			return
		default:
			select {
			case <-b.out:
			default:
			}
		}
	}
}

// mix sums one frame from every speaker with audio queued. nil when nobody
// is talking.
func (b *Bridge) mix() []int16 {
	b.mu.Lock()
	defer b.mu.Unlock()
	var acc []int32
	for _, s := range b.users {
		if len(s.frames) == 0 {
			continue
		}
		f := s.frames[0]
		s.frames = s.frames[1:]
		if acc == nil {
			acc = make([]int32, frameSamples)
		}
		for i := 0; i < frameSamples && i < len(f); i++ {
			acc[i] += int32(f[i])
		}
	}
	if acc == nil {
		return nil
	}
	out := make([]int16, frameSamples)
	for i, v := range acc {
		out[i] = int16(min(max(v, -32768), 32767))
	}
	return out
}

// Stop ends the clock and releases the codecs.
func (b *Bridge) Stop() {
	select {
	case <-b.stop:
		return
	default:
		close(b.stop)
	}
	<-b.done
	b.mu.Lock()
	for id, s := range b.users {
		s.dec.Destroy()
		delete(b.users, id)
	}
	b.mu.Unlock()
	b.enc.Destroy()
}

// Provider returns the voice.OpusFrameProvider to give the voice connection.
func (b *Bridge) Provider() voice.OpusFrameProvider { return (*bridgeProvider)(b) }

// Receiver returns the voice.OpusFrameReceiver to give the voice connection.
func (b *Bridge) Receiver() voice.OpusFrameReceiver { return (*bridgeReceiver)(b) }

type bridgeProvider Bridge

// ProvideOpusFrame is pulled by disgo every 20 ms. Returning nil makes disgo
// send a few silence frames and then stop speaking.
func (p *bridgeProvider) ProvideOpusFrame() ([]byte, error) {
	select {
	case f := <-p.out:
		p.framesToDiscord.Add(1)
		return f, nil
	default:
		return nil, nil
	}
}

func (p *bridgeProvider) Close() {}

type bridgeReceiver Bridge

func (r *bridgeReceiver) ReceiveOpusFrame(userID snowflake.ID, packet *voice.Packet) error {
	if userID == 0 || userID == r.self || (r.allow != nil && !r.allow(userID)) {
		return nil
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.users[userID]
	if !ok {
		dec, err := opus.NewDecoder(sampleRate, 1)
		if err != nil {
			return err
		}
		s = &speaker{dec: dec}
		r.users[userID] = s
	}
	// Up to 120 ms per packet at 8 kHz.
	pcm := make([]int16, 960)
	n, err := s.dec.Decode(packet.Opus, pcm, false)
	if err != nil {
		return err
	}
	for off := 0; off+frameSamples <= n; off += frameSamples {
		s.frames = append(s.frames, pcm[off:off+frameSamples])
	}
	if over := len(s.frames) - userQueue; over > 0 {
		s.frames = s.frames[over:]
	}
	return nil
}

func (r *bridgeReceiver) CleanupUser(userID snowflake.ID) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.users[userID]; ok {
		s.dec.Destroy()
		delete(r.users, userID)
	}
}

func (r *bridgeReceiver) Close() {}
