package ukcp

import "time"

const (
	defaultPacketSize      = 2048
	defaultSessionQueue    = 128
	defaultUDPDedupeWindow = 512
	defaultUpdateInterval  = 10 * time.Millisecond
	defaultFastReconnect   = 10 * time.Second
	defaultTransportMTU    = 1024
)

type KCPConfig struct {
	MTU          int
	NoDelay      int
	Interval     int
	Resend       int
	NoCongestion int
	SendWindow   int
	RecvWindow   int
	AckNoDelay   bool
}

type Config struct {
	PacketSize          int
	SessionQueueSize    int
	UDPDedupeWindow     uint32
	UpdateInterval      time.Duration
	FastReconnectWindow time.Duration
	KCP                 KCPConfig
}

func (c Config) withDefaults() Config {
	if c.PacketSize <= 0 {
		c.PacketSize = defaultPacketSize
	}
	if c.SessionQueueSize <= 0 {
		c.SessionQueueSize = defaultSessionQueue
	}
	if c.UDPDedupeWindow == 0 {
		c.UDPDedupeWindow = defaultUDPDedupeWindow
	}
	if c.UpdateInterval <= 0 {
		c.UpdateInterval = defaultUpdateInterval
	}
	if c.FastReconnectWindow <= 0 {
		c.FastReconnectWindow = defaultFastReconnect
	}
	if c.KCP.MTU <= 0 {
		c.KCP.MTU = defaultTransportMTU
	}
	if c.KCP.NoDelay == 0 {
		c.KCP.NoDelay = 1
	}
	if c.KCP.Interval == 0 {
		c.KCP.Interval = 10
	}
	if c.KCP.Resend == 0 {
		c.KCP.Resend = 2
	}
	if c.KCP.NoCongestion == 0 {
		c.KCP.NoCongestion = 1
	}
	if c.KCP.SendWindow <= 0 {
		c.KCP.SendWindow = 128
	}
	if c.KCP.RecvWindow <= 0 {
		c.KCP.RecvWindow = 128
	}
	return c
}
