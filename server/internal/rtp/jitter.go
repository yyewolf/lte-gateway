package rtp

import "sync"

// FrameSamples is one 20 ms frame at 8 kHz.
const FrameSamples = 160

// Jitter is a small reordering jitter buffer for fixed 20 ms frames, keyed by RTP sequence number.
// Push is called from the network side, Pop on a 20 ms clock by the consumer.
type Jitter struct {
	mu       sync.Mutex
	frames   map[uint16][]int16
	next     uint16 // next sequence number to play
	started  bool   // next is valid
	playing  bool
	prefill  int
	max      int
	lastPush uint16

	Late, Lost, Underruns, Overflows uint64
}

// NewJitter starts playing once prefill frames are queued and never holds more than max.
func NewJitter(prefill, max int) *Jitter {
	return &Jitter{frames: map[uint16][]int16{}, prefill: prefill, max: max}
}

// Reset drops everything (new call).
func (j *Jitter) Reset() {
	j.mu.Lock()
	defer j.mu.Unlock()
	j.frames = map[uint16][]int16{}
	j.started, j.playing = false, false
}

// Push queues a frame. Frames older than what was already played are dropped.
func (j *Jitter) Push(seq uint16, frame []int16) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if !j.started {
		j.next, j.started = seq, true
	}
	if int16(seq-j.next) < 0 {
		j.Late++
		return
	}
	// A jump far ahead (sender restarted): start over from here.
	if int16(seq-j.next) > int16(4*j.max) {
		j.frames = map[uint16][]int16{}
		j.next, j.playing = seq, false
	}
	j.frames[seq] = frame
	j.lastPush = seq
	for len(j.frames) > j.max { // too much buffered: skip ahead
		if _, ok := j.frames[j.next]; ok {
			delete(j.frames, j.next)
			j.Overflows++
		} else {
			j.Lost++
		}
		j.next++
	}
}

// Pop returns the next frame, or nil while buffering / when the frame is missing.
func (j *Jitter) Pop() []int16 {
	j.mu.Lock()
	defer j.mu.Unlock()
	if !j.started {
		return nil
	}
	if !j.playing {
		if len(j.frames) < j.prefill {
			return nil
		}
		j.playing = true
	}
	if len(j.frames) == 0 {
		j.playing = false // ran dry: refill before playing again
		j.Underruns++
		return nil
	}
	f, ok := j.frames[j.next]
	if ok {
		delete(j.frames, j.next)
	} else {
		j.Lost++ // gap: play silence for this slot
	}
	j.next++
	return f
}

// Len is the number of queued frames.
func (j *Jitter) Len() int {
	j.mu.Lock()
	defer j.mu.Unlock()
	return len(j.frames)
}
