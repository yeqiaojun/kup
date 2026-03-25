package ukcp

import "ukcp/protocol"

const (
	kcpOverhead            = 24
	kcpMinMtu              = 50
	maxKcpMessageFragments = 127
	maxPacketBodyLen       = int(^uint16(0))
)

func kcpMtuFromTransportMtu(transportMtu int) int {
	return transportMtu - protocol.HeaderSize
}

func maxKcpPayloadForTransportMtu(transportMtu int) int {
	kcpMtu := kcpMtuFromTransportMtu(transportMtu)
	if kcpMtu < kcpMinMtu {
		return 0
	}

	mss := kcpMtu - kcpOverhead
	if mss <= 0 {
		return 0
	}
	return mss * maxKcpMessageFragments
}
