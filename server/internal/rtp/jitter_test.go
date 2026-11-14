package rtp

import "testing"

func frame(v int16) []int16 {
	f := make([]int16, FrameSamples)
	f[0] = v
	return f
}

func TestPrefillAndOrder(t *testing.T) {
	j := NewJitter(3, 10)
	j.Push(100, frame(1))
	j.Push(102, frame(3)) // out of order
	if j.Pop() != nil {
		t.Fatal("played before prefill")
	}
	j.Push(101, frame(2))
	for want := int16(1); want <= 3; want++ {
		f := j.Pop()
		if f == nil || f[0] != want {
			t.Fatalf("want frame %d, got %v", want, f)
		}
	}
	if j.Pop() != nil || j.Underruns != 1 {
		t.Fatalf("expected an underrun, got %d", j.Underruns)
	}
}

func TestLateAndGap(t *testing.T) {
	j := NewJitter(1, 10)
	j.Push(10, frame(1))
	j.Pop()
	j.Push(9, frame(9)) // older than played
	if j.Late != 1 {
		t.Fatal("late frame not counted")
	}
	j.Push(12, frame(3)) // 11 lost
	if f := j.Pop(); f != nil || j.Lost != 1 {
		t.Fatalf("gap should play silence, got %v lost=%d", f, j.Lost)
	}
	if f := j.Pop(); f == nil || f[0] != 3 {
		t.Fatalf("want frame 3, got %v", f)
	}
}

func TestOverflowAndWrap(t *testing.T) {
	j := NewJitter(1, 4)
	for s := uint16(65530); s != 4; s++ { // wraps around 0
		j.Push(s, frame(int16(s)))
	}
	if j.Len() > 4 {
		t.Fatalf("buffer holds %d > max", j.Len())
	}
	f := j.Pop()
	if f == nil || uint16(f[0]) != 0 {
		t.Fatalf("after overflow expect the newest 4 frames starting at 0, got %v", f)
	}
}

func TestRestartJump(t *testing.T) {
	j := NewJitter(1, 4)
	j.Push(1, frame(1))
	j.Pop()
	j.Push(30000, frame(7))
	if f := j.Pop(); f == nil || f[0] != 7 {
		t.Fatalf("expected resync to the new stream, got %v", f)
	}
}
