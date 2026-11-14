package g711

import "testing"

func TestReferencePoints(t *testing.T) {
	cases := []struct {
		in   int16
		want byte
	}{{0, 0xFF}, {32767, 0x80}, {-32768, 0x00}}
	for _, c := range cases {
		if got := EncodeUlaw(c.in); got != c.want {
			t.Errorf("EncodeUlaw(%d) = %#x, want %#x", c.in, got, c.want)
		}
	}
	if got := DecodeUlaw(0x80); got != 32124 {
		t.Errorf("DecodeUlaw(0x80) = %d", got)
	}
	if got := DecodeAlaw(0xD5); got != 8 {
		t.Errorf("DecodeAlaw(0xD5) = %d", got)
	}
	if got := DecodeAlaw(0xAA); got != 32256 {
		t.Errorf("DecodeAlaw(0xAA) = %d", got)
	}
}

func TestUlawCodesRoundTrip(t *testing.T) {
	for u := 0; u < 256; u++ {
		r := EncodeUlaw(DecodeUlaw(byte(u)))
		if r != byte(u) && !(u == 0x7F && r == 0xFF) {
			t.Errorf("code %#x -> %#x", u, r)
		}
	}
}

func TestUlawError(t *testing.T) {
	for x := -32124; x <= 32124; x++ {
		y := int(DecodeUlaw(EncodeUlaw(int16(x))))
		d := y - x
		if d < 0 {
			d = -d
		}
		ax := x
		if ax < 0 {
			ax = -ax
		}
		if d > ax/16+8 {
			t.Fatalf("%d -> %d", x, y)
		}
	}
}
