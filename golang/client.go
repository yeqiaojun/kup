package ukcp

import (
	"errors"
	"net"
	"sync"
	"time"

	kcp "github.com/xtaci/kcp-go/v5"

	"ukcp/protocol"
)

var ErrClientClosed = errors.New("ukcp: client closed")

type Client struct {
	serverAddr net.Addr
	sessID     uint32
	config     Config

	kcp *kcp.KCP

	recvCh chan []byte
	events chan clientEvent
	closed chan struct{}

	connMu sync.RWMutex
	conn   net.PacketConn

	closeOnce sync.Once
	wg        sync.WaitGroup

	outputFlags protocol.Flags
}

type clientEventKind uint8

const (
	clientEventInbound clientEventKind = iota + 1
	clientEventSendKCP
)

type clientEvent struct {
	kind    clientEventKind
	msgType protocol.MsgType
	payload []byte
	flags   protocol.Flags
}

func Dial(addr string, sessID uint32, config Config) (*Client, error) {
	serverAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}

	conn, err := net.ListenPacket("udp", ":0")
	if err != nil {
		return nil, err
	}

	cfg := config.withDefaults()
	client := &Client{
		conn:       conn,
		serverAddr: serverAddr,
		sessID:     sessID,
		config:     cfg,
		recvCh:     make(chan []byte, cfg.SessionQueueSize),
		events:     make(chan clientEvent, cfg.SessionQueueSize),
		closed:     make(chan struct{}),
	}
	client.kcp = kcp.NewKCP(sessID, func(buf []byte, size int) {
		client.writeKCP(buf[:size])
	})
	client.kcp.NoDelay(cfg.KCP.NoDelay, cfg.KCP.Interval, cfg.KCP.Resend, cfg.KCP.NoCongestion)
	client.kcp.WndSize(cfg.KCP.SendWindow, cfg.KCP.RecvWindow)
	kcpMTU := kcpMtuFromTransportMtu(cfg.KCP.MTU)
	if kcpMTU >= kcpMinMtu {
		_ = client.kcp.SetMtu(kcpMTU)
	}

	client.wg.Add(2)
	go client.readLoop()
	go client.loop()
	return client, nil
}

func (c *Client) SendUdp(packetSeq uint32, payload []byte) error {
	if c.isClosed() {
		return ErrClientClosed
	}
	if len(payload) > maxPacketBodyLen {
		return ErrPayloadTooLarge
	}

	header, err := (protocol.Header{
		MsgType:   protocol.MsgTypeUDP,
		BodyLen:   uint16(len(payload)),
		SessID:    c.sessID,
		PacketSeq: packetSeq,
	}).MarshalBinary()
	if err != nil {
		return err
	}

	packet := make([]byte, len(header)+len(payload))
	copy(packet, header)
	copy(packet[len(header):], payload)
	conn := c.packetConn()
	if conn == nil {
		return ErrClientClosed
	}
	if _, err := conn.WriteTo(packet, c.serverAddr); err != nil {
		return err
	}
	return nil
}

func (c *Client) SendKcp(payload []byte) error {
	return c.sendKCPEvent(payload, protocol.FlagNone)
}

func (c *Client) SendAuth(payload []byte) error {
	return c.sendKCPEvent(payload, protocol.FlagConnect)
}

func (c *Client) sendKCPEvent(payload []byte, flags protocol.Flags) error {
	if c.isClosed() {
		return ErrClientClosed
	}
	if len(payload) > maxKcpPayloadForTransportMtu(c.config.KCP.MTU) {
		return ErrKcpPayloadTooLarge
	}

	body := append([]byte(nil), payload...)
	select {
	case c.events <- clientEvent{kind: clientEventSendKCP, payload: body, flags: flags}:
		return nil
	case <-c.closed:
		return ErrClientClosed
	}
}

func (c *Client) Recv() <-chan []byte {
	return c.recvCh
}

func (c *Client) Reconnect() error {
	if c.isClosed() {
		return ErrClientClosed
	}

	newConn, err := net.ListenPacket("udp", ":0")
	if err != nil {
		return err
	}

	old := c.swapConn(newConn)
	if old != nil {
		_ = old.Close()
	}
	return nil
}

func (c *Client) Close() error {
	var err error
	c.closeOnce.Do(func() {
		close(c.closed)
		if conn := c.swapConn(nil); conn != nil {
			err = conn.Close()
		}
		c.wg.Wait()
		close(c.recvCh)
	})
	return err
}

func (c *Client) readLoop() {
	defer c.wg.Done()

	buf := make([]byte, c.config.PacketSize)
	for {
		conn := c.packetConn()
		if conn == nil {
			if c.isClosed() {
				return
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		n, _, err := conn.ReadFrom(buf)
		if err != nil {
			if c.isClosed() {
				return
			}
			if conn != c.packetConn() {
				continue
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		header, body, err := protocol.SplitPacket(buf[:n])
		if err != nil || header.SessID != c.sessID {
			continue
		}
		packet := append([]byte(nil), body...)
		select {
		case c.events <- clientEvent{kind: clientEventInbound, msgType: header.MsgType, payload: packet}:
		case <-c.closed:
			return
		}
	}
}

func (c *Client) loop() {
	defer c.wg.Done()

	ticker := time.NewTicker(c.config.UpdateInterval)
	defer ticker.Stop()

	for {
		select {
		case <-c.closed:
			return
		case <-ticker.C:
			c.kcp.Update()
			c.drainKCP()
		case event := <-c.events:
			switch event.kind {
			case clientEventInbound:
				if event.msgType == protocol.MsgTypeUDP {
					select {
					case c.recvCh <- event.payload:
					case <-c.closed:
						return
					}
					break
				}
				if rc := c.kcp.Input(event.payload, kcp.IKCP_PACKET_REGULAR, c.config.KCP.AckNoDelay); rc == 0 {
					c.drainKCP()
				}
			case clientEventSendKCP:
				c.sendKcp(event.payload, event.flags)
			}
		}
	}
}

func (c *Client) drainKCP() {
	for {
		size := c.kcp.PeekSize()
		if size < 0 {
			return
		}

		buf := make([]byte, size)
		n := c.kcp.Recv(buf)
		if n < 0 {
			return
		}

		payload := append([]byte(nil), buf[:n]...)
		select {
		case c.recvCh <- payload:
		case <-c.closed:
			return
		}
	}
}

func (c *Client) sendKcp(payload []byte, flags protocol.Flags) {
	c.outputFlags = flags
	defer func() {
		c.outputFlags = protocol.FlagNone
	}()

	if c.kcp.Send(payload) == 0 {
		c.kcp.Update()
	}
}

func (c *Client) writeKCP(payload []byte) {
	if c.isClosed() {
		return
	}

	conn := c.packetConn()
	if conn == nil {
		return
	}

	header, err := (protocol.Header{
		MsgType: protocol.MsgTypeKCP,
		Flags:   c.outputFlags,
		BodyLen: uint16(len(payload)),
		SessID:  c.sessID,
	}).MarshalBinary()
	if err != nil {
		return
	}

	packet := make([]byte, len(header)+len(payload))
	copy(packet, header)
	copy(packet[len(header):], payload)
	_, _ = conn.WriteTo(packet, c.serverAddr)
}

func (c *Client) isClosed() bool {
	select {
	case <-c.closed:
		return true
	default:
		return false
	}
}

func (c *Client) packetConn() net.PacketConn {
	c.connMu.RLock()
	defer c.connMu.RUnlock()
	return c.conn
}

func (c *Client) swapConn(conn net.PacketConn) net.PacketConn {
	c.connMu.Lock()
	defer c.connMu.Unlock()
	old := c.conn
	c.conn = conn
	return old
}
