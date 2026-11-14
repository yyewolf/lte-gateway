package recorder

import (
	"encoding/binary"
	"os"
	"testing"
	"time"
)

func TestRecorder(t *testing.T) {
	dir := t.TempDir()
	r, err := Start(dir, "incoming", "+33 6/12", time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC))
	if err != nil {
		t.Fatal(err)
	}
	phone := make([]int16, 160)
	for i := range phone {
		phone[i] = int16(i)
	}
	for i := 0; i < 50; i++ { // 1 s
		r.Write(phone, nil)
	}
	dur, err := r.Close()
	if err != nil {
		t.Fatal(err)
	}
	if dur != time.Second {
		t.Fatalf("duration %v", dur)
	}
	if r.Path != dir+"/20260102-030405_incoming_+33612.wav" {
		t.Fatalf("path %s", r.Path)
	}
	b, err := os.ReadFile(r.Path)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != 44+8000*4 {
		t.Fatalf("size %d", len(b))
	}
	if got := binary.LittleEndian.Uint32(b[40:]); got != 8000*4 {
		t.Fatalf("data size %d", got)
	}
	if got := int16(binary.LittleEndian.Uint16(b[44+4*5:])); got != 5 {
		t.Fatalf("sample %d", got)
	}
}

func TestShortRecordingRemoved(t *testing.T) {
	r, err := Start(t.TempDir(), "outgoing", "123", time.Now())
	if err != nil {
		t.Fatal(err)
	}
	r.Write(make([]int16, 160), nil)
	r.Close()
	if _, err := os.Stat(r.Path); !os.IsNotExist(err) {
		t.Fatal("short recording kept")
	}
}
