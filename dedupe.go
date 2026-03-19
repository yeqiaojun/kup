package ukcp

type seqWindow struct {
	size        uint32
	highest     uint32
	initialized bool
	bits        []uint64
}

func newSeqWindow(size uint32) *seqWindow {
	if size < 64 {
		size = 64
	}
	words := int((size + 63) / 64)
	return &seqWindow{
		size: size,
		bits: make([]uint64, words),
	}
}

func (w *seqWindow) Seen(seq uint32) bool {
	if !w.initialized {
		w.initialized = true
		w.highest = seq
		w.bits[0] = 1
		return false
	}

	diff := int32(seq - w.highest)
	if diff > 0 {
		w.shift(uint32(diff))
		w.highest = seq
		w.bits[0] |= 1
		return false
	}

	offset := uint32(-diff)
	if offset >= w.size {
		return true
	}

	word := offset / 64
	bit := offset % 64
	mask := uint64(1) << bit
	if w.bits[word]&mask != 0 {
		return true
	}
	w.bits[word] |= mask
	return false
}

func (w *seqWindow) shift(delta uint32) {
	if delta >= w.size {
		clear(w.bits)
		return
	}

	wordShift := int(delta / 64)
	bitShift := delta % 64

	for i := len(w.bits) - 1; i >= 0; i-- {
		var next uint64
		from := i - wordShift
		if from >= 0 {
			next = w.bits[from] << bitShift
			if bitShift > 0 && from > 0 {
				next |= w.bits[from-1] >> (64 - bitShift)
			}
		}
		w.bits[i] = next
	}

	excess := uint32(len(w.bits)*64) - w.size
	if excess > 0 {
		w.bits[len(w.bits)-1] &= ^uint64(0) >> excess
	}
}
