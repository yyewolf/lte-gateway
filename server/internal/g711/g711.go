// Package g711 converts between 16-bit linear PCM and G.711 µ-law / A-law (ITU-T G.711).
package g711

// EncodeUlaw converts one linear sample to µ-law.
func EncodeUlaw(pcm int16) byte {
	const bias, clip = 0x84, 32635
	s := int(pcm)
	sign := 0
	if s < 0 {
		sign = 0x80
		s = -s
	}
	if s > clip {
		s = clip
	}
	s += bias
	exp := 7
	for mask := 0x4000; s&mask == 0 && exp > 0; mask >>= 1 {
		exp--
	}
	mant := (s >> (exp + 3)) & 0x0F
	return ^byte(sign | exp<<4 | mant)
}

// DecodeUlaw converts one µ-law byte to linear PCM.
func DecodeUlaw(u byte) int16 {
	u = ^u
	t := (int(u&0x0F) << 3) + 0x84
	t <<= (u & 0x70) >> 4
	if u&0x80 != 0 {
		return int16(0x84 - t)
	}
	return int16(t - 0x84)
}

// DecodeAlaw converts one A-law byte to linear PCM.
func DecodeAlaw(a byte) int16 {
	a ^= 0x55
	t := int(a&0x0F) << 4
	seg := int(a&0x70) >> 4
	switch seg {
	case 0:
		t += 8
	case 1:
		t += 0x108
	default:
		t = (t + 0x108) << (seg - 1)
	}
	if a&0x80 != 0 {
		return int16(t)
	}
	return int16(-t)
}

// EncodeUlawFrame encodes pcm into dst (len(dst) >= len(pcm)).
func EncodeUlawFrame(dst []byte, pcm []int16) {
	for i, s := range pcm {
		dst[i] = EncodeUlaw(s)
	}
}
