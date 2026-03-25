package protocol

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func TestHeaderMarshalBinary(t *testing.T) {
	h := Header{
		MsgType:   MsgTypeKCP,
		Flags:     FlagConnect,
		BodyLen:   5,
		SessID:    0x11223344,
		PacketSeq: 0x55667788,
	}

	got, err := h.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}

	want, err := hex.DecodeString("020105004433221188776655")
	if err != nil {
		t.Fatalf("hex.DecodeString() error = %v", err)
	}

	if !bytes.Equal(got, want) {
		t.Fatalf("MarshalBinary() = %x, want %x", got, want)
	}
}

func TestUnmarshalHeader(t *testing.T) {
	packet, err := hex.DecodeString("01000300040302010d0c0b0a")
	if err != nil {
		t.Fatalf("hex.DecodeString() error = %v", err)
	}

	got, err := UnmarshalHeader(packet)
	if err != nil {
		t.Fatalf("UnmarshalHeader() error = %v", err)
	}

	want := Header{
		MsgType:   MsgTypeUDP,
		Flags:     0,
		BodyLen:   3,
		SessID:    0x01020304,
		PacketSeq: 0x0a0b0c0d,
	}

	if got != want {
		t.Fatalf("UnmarshalHeader() = %+v, want %+v", got, want)
	}
}

func TestSplitPacket(t *testing.T) {
	packet, err := hex.DecodeString("01000300040302010d0c0b0aaabbcc")
	if err != nil {
		t.Fatalf("hex.DecodeString() error = %v", err)
	}

	h, body, err := SplitPacket(packet)
	if err != nil {
		t.Fatalf("SplitPacket() error = %v", err)
	}

	if h.MsgType != MsgTypeUDP {
		t.Fatalf("SplitPacket() header.MsgType = %d, want %d", h.MsgType, MsgTypeUDP)
	}

	if !bytes.Equal(body, []byte{0xaa, 0xbb, 0xcc}) {
		t.Fatalf("SplitPacket() body = %x, want %x", body, []byte{0xaa, 0xbb, 0xcc})
	}
}

func TestUnmarshalHeaderRejectsShortPacket(t *testing.T) {
	_, err := UnmarshalHeader([]byte{0x01})
	if err == nil {
		t.Fatal("UnmarshalHeader() error = nil, want error")
	}
}

func TestUnmarshalHeaderRejectsInvalidMsgType(t *testing.T) {
	packet, err := hex.DecodeString("03000000040302010d0c0b0a")
	if err != nil {
		t.Fatalf("hex.DecodeString() error = %v", err)
	}

	_, err = UnmarshalHeader(packet)
	if err == nil {
		t.Fatal("UnmarshalHeader() error = nil, want error")
	}
}

func TestSplitPacketRejectsLengthMismatch(t *testing.T) {
	packet, err := hex.DecodeString("01000400040302010d0c0b0aaabbcc")
	if err != nil {
		t.Fatalf("hex.DecodeString() error = %v", err)
	}

	_, _, err = SplitPacket(packet)
	if err == nil {
		t.Fatal("SplitPacket() error = nil, want error")
	}
}
