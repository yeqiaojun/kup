package protocol

import (
	"errors"
)

const HeaderSize = 12

type MsgType uint8
type Flags uint8

const (
	MsgTypeUDP MsgType = 1
	MsgTypeKCP MsgType = 2
)

const (
	FlagNone    Flags = 0
	FlagConnect Flags = 1 << 0
)

var (
	ErrShortPacket        = errors.New("protocol: packet shorter than header")
	ErrInvalidMsgType     = errors.New("protocol: invalid msg type")
	ErrPayloadLenMismatch = errors.New("protocol: payload length mismatch")
)

type Header struct {
	MsgType   MsgType
	Flags     Flags
	BodyLen   uint16
	SessID    uint32
	PacketSeq uint32
}

func (h Header) MarshalBinary() ([]byte, error) {
	if !isValidMsgType(h.MsgType) {
		return nil, ErrInvalidMsgType
	}

	buf := make([]byte, HeaderSize)
	buf[0] = byte(h.MsgType)
	buf[1] = byte(h.Flags)
	putUint16LE(buf[2:4], h.BodyLen)
	putUint32LE(buf[4:8], h.SessID)
	putUint32LE(buf[8:12], h.PacketSeq)
	return buf, nil
}

func UnmarshalHeader(packet []byte) (Header, error) {
	if len(packet) < HeaderSize {
		return Header{}, ErrShortPacket
	}

	h := Header{
		MsgType:   MsgType(packet[0]),
		Flags:     Flags(packet[1]),
		BodyLen:   readUint16LE(packet[2:4]),
		SessID:    readUint32LE(packet[4:8]),
		PacketSeq: readUint32LE(packet[8:12]),
	}
	if !isValidMsgType(h.MsgType) {
		return Header{}, ErrInvalidMsgType
	}

	return h, nil
}

func SplitPacket(packet []byte) (Header, []byte, error) {
	h, err := UnmarshalHeader(packet)
	if err != nil {
		return Header{}, nil, err
	}

	if len(packet) != HeaderSize+int(h.BodyLen) {
		return Header{}, nil, ErrPayloadLenMismatch
	}

	return h, packet[HeaderSize:], nil
}

func isValidMsgType(msgType MsgType) bool {
	return msgType == MsgTypeUDP || msgType == MsgTypeKCP
}

func putUint16LE(buf []byte, value uint16) {
	buf[0] = byte(value)
	buf[1] = byte(value >> 8)
}

func putUint32LE(buf []byte, value uint32) {
	buf[0] = byte(value)
	buf[1] = byte(value >> 8)
	buf[2] = byte(value >> 16)
	buf[3] = byte(value >> 24)
}

func readUint16LE(buf []byte) uint16 {
	return uint16(buf[0]) | uint16(buf[1])<<8
}

func readUint32LE(buf []byte) uint32 {
	return uint32(buf[0]) |
		uint32(buf[1])<<8 |
		uint32(buf[2])<<16 |
		uint32(buf[3])<<24
}
