// Package recorder writes call audio to a stereo 8 kHz 16-bit WAV file:
// left channel is the phone side, right channel is the Discord side.
package recorder

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sync"
	"time"
)

const SampleRate = 8000

type Recorder struct {
	mu      sync.Mutex
	f       *os.File
	w       *bufio.Writer
	samples uint32 // stereo frames written
	Path    string
}

var unsafeChars = regexp.MustCompile(`[^0-9A-Za-z+_-]`)

// Start creates dir/<time>_<direction>_<number>.wav.
func Start(dir, direction, number string, now time.Time) (*Recorder, error) {
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return nil, err
	}
	name := fmt.Sprintf("%s_%s_%s.wav", now.Format("20060102-150405"), direction, unsafeChars.ReplaceAllString(number, ""))
	path := filepath.Join(dir, name)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o640)
	if err != nil {
		return nil, err
	}
	r := &Recorder{f: f, w: bufio.NewWriterSize(f, 64<<10), Path: path}
	if err := r.header(0); err != nil {
		f.Close()
		return nil, err
	}
	return r, nil
}

func (r *Recorder) header(frames uint32) error {
	data := frames * 4
	h := make([]byte, 44)
	copy(h[0:], "RIFF")
	binary.LittleEndian.PutUint32(h[4:], 36+data)
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1) // PCM
	binary.LittleEndian.PutUint16(h[22:], 2) // channels
	binary.LittleEndian.PutUint32(h[24:], SampleRate)
	binary.LittleEndian.PutUint32(h[28:], SampleRate*4)
	binary.LittleEndian.PutUint16(h[32:], 4)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], data)
	_, err := r.w.Write(h)
	return err
}

// Write appends one frame. Either side may be nil (silence); the frame length
// is the longer of the two.
func (r *Recorder) Write(phone, discord []int16) {
	n := max(len(phone), len(discord))
	if n == 0 {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.w == nil {
		return
	}
	var b [4]byte
	for i := 0; i < n; i++ {
		var l, rr int16
		if i < len(phone) {
			l = phone[i]
		}
		if i < len(discord) {
			rr = discord[i]
		}
		binary.LittleEndian.PutUint16(b[0:], uint16(l))
		binary.LittleEndian.PutUint16(b[2:], uint16(rr))
		r.w.Write(b[:])
	}
	r.samples += uint32(n)
}

// Close finalises the header and closes the file. Recordings shorter than
// half a second are removed.
func (r *Recorder) Close() (time.Duration, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.w == nil {
		return 0, nil
	}
	dur := time.Duration(r.samples) * time.Second / SampleRate
	err := r.w.Flush()
	r.w = nil
	if err == nil {
		if _, err = r.f.Seek(0, 0); err == nil {
			hw := bufio.NewWriter(r.f)
			tmp := &Recorder{w: hw}
			if err = tmp.header(r.samples); err == nil {
				err = hw.Flush()
			}
		}
	}
	if cerr := r.f.Close(); err == nil {
		err = cerr
	}
	if dur < 500*time.Millisecond {
		os.Remove(r.Path)
	}
	return dur, err
}
