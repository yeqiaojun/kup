package ukcp

import (
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"

	kcp "github.com/xtaci/kcp-go/v5"

	"ukcp/protocol"
)

var (
	ErrSessionClosed     = errors.New("ukcp: session closed")
	ErrSessionReplaced   = errors.New("ukcp: session replaced")
	ErrSessionQueueFull  = errors.New("ukcp: session queue full")
	ErrRemoteAddrUnknown = errors.New("ukcp: remote addr unknown")
)

type Session struct {
	id        uint32
	server    *Server
	kcp       *kcp.KCP
	transport *sessionTransport

	events    chan sessionEvent
	closeCh   chan error
	closeOnce sync.Once
	closed    chan struct{}
	lastKCPAt atomic.Int64
	recvBuf   []byte
}

type sessionEventKind uint8

const (
	sessionEventInbound sessionEventKind = iota + 1
	sessionEventSend
	sessionEventDrain
)

type inboundPacket struct {
	msgType   protocol.MsgType
	packetSeq uint32
	payload   []byte
	addr      net.Addr
}

type sessionEvent struct {
	kind    sessionEventKind
	inbound inboundPacket
	payload []byte
}

func newSession(server *Server, id uint32, transport *sessionTransport, state *kcp.KCP) *Session {
	if transport == nil {
		transport = newSessionTransport(server, id, nil)
	}
	sess := &Session{
		id:        id,
		server:    server,
		transport: transport,
		events:    make(chan sessionEvent, server.config.SessionQueueSize),
		closeCh:   make(chan error, 1),
		closed:    make(chan struct{}),
	}
	if state != nil {
		sess.kcp = state
	} else {
		sess.kcp = newKCPState(server, id, transport)
	}
	sess.markKCPActive(time.Now())

	server.wg.Add(1)
	go sess.loop()
	return sess
}

func (s *Session) ID() uint32 {
	return s.id
}

func (s *Session) RemoteAddr() net.Addr {
	return s.transport.addr()
}

func (s *Session) Send(payload []byte) error {
	if s.server.isClosed() {
		return ErrServerClosed
	}
	if s.isClosed() {
		return ErrSessionClosed
	}
	if s.RemoteAddr() == nil {
		return ErrRemoteAddrUnknown
	}

	body := append([]byte(nil), payload...)
	select {
	case s.events <- sessionEvent{kind: sessionEventSend, payload: body}:
		return nil
	default:
		return ErrSessionQueueFull
	}
}

func (s *Session) Close() error {
	return s.closeWithReason(ErrSessionClosed)
}

func (s *Session) closeWithReason(reason error) error {
	s.closeOnce.Do(func() {
		s.closeCh <- reason
	})
	return nil
}

func (s *Session) handleInbound(packet inboundPacket) {
	select {
	case <-s.closed:
		return
	default:
	}

	select {
	case s.events <- sessionEvent{kind: sessionEventInbound, inbound: packet}:
	default:
	}
}

func (s *Session) loop() {
	defer s.server.wg.Done()

	timer := time.NewTimer(s.nextUpdateDelay())
	defer timer.Stop()

	closeErr := ErrSessionClosed

	defer func() {
		close(s.closed)
		s.server.removeSession(s.id, s)
		s.server.handler.OnSessionClose(s, closeErr)
	}()

	for {
		select {
		case <-s.server.closed:
			closeErr = ErrServerClosed
			return
		case closeErr = <-s.closeCh:
			return
		case <-timer.C:
			s.kcp.Update()
			s.drainKCP()
			resetTimer(timer, s.nextUpdateDelay())
		case event := <-s.events:
			switch event.kind {
			case sessionEventInbound:
				s.transport.setAddr(event.inbound.addr)
				switch event.inbound.msgType {
				case protocol.MsgTypeUDP:
					s.server.handler.OnUDP(s, event.inbound.packetSeq, event.inbound.payload)
				case protocol.MsgTypeKCP:
					if rc := s.kcp.Input(event.inbound.payload, kcp.IKCP_PACKET_REGULAR, s.server.config.KCP.AckNoDelay); rc == 0 {
						s.markKCPActive(time.Now())
						s.kcp.Update()
						s.drainKCP()
					}
				}
			case sessionEventSend:
				if s.kcp.Send(event.payload) == 0 {
					s.kcp.Update()
					s.drainKCP()
				}
			case sessionEventDrain:
				s.drainKCP()
			}
			resetTimer(timer, s.nextUpdateDelay())
		}
	}
}

func (s *Session) writeKCP(payload []byte) {
	s.transport.writeKCP(payload)
}

func (s *Session) drainKCP() {
	for {
		size := s.kcp.PeekSize()
		if size < 0 {
			return
		}

		buf := ensureScratch(s.recvBuf, size)
		s.recvBuf = buf
		n := s.kcp.Recv(buf)
		if n < 0 {
			return
		}
		s.server.handler.OnKCP(s, buf[:n])
	}
}

func (s *Session) isClosed() bool {
	select {
	case <-s.closed:
		return true
	default:
		return false
	}
}

func (s *Session) kickDrain() {
	select {
	case s.events <- sessionEvent{kind: sessionEventDrain}:
	default:
	}
}

func (s *Session) withinFastReconnectWindow(now time.Time) bool {
	window := s.server.config.FastReconnectWindow
	if window <= 0 {
		return false
	}

	last := s.lastKCPAt.Load()
	if last == 0 {
		return false
	}
	return now.Sub(time.Unix(0, last)) <= window
}

func (s *Session) markKCPActive(now time.Time) {
	s.lastKCPAt.Store(now.UnixNano())
}

func (s *Session) nextUpdateDelay() time.Duration {
	next := s.kcp.Check()
	now := uint32(time.Now().UnixMilli())
	if next <= now {
		return 0
	}
	return time.Duration(next-now) * time.Millisecond
}

type sessionTransport struct {
	server *Server
	sessID uint32

	mu         sync.RWMutex
	remoteAddr net.Addr
}

func newSessionTransport(server *Server, sessID uint32, addr net.Addr) *sessionTransport {
	return &sessionTransport{
		server:     server,
		sessID:     sessID,
		remoteAddr: addr,
	}
}

func (t *sessionTransport) setAddr(addr net.Addr) {
	t.mu.Lock()
	t.remoteAddr = addr
	t.mu.Unlock()
}

func (t *sessionTransport) addr() net.Addr {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.remoteAddr
}

func (t *sessionTransport) writeKCP(payload []byte) {
	addr := t.addr()
	if addr == nil || t.server.isClosed() {
		return
	}

	size := protocol.HeaderSize + len(payload)
	packet := t.server.acquirePacketBuffer(size)
	header := protocol.Header{
		MsgType: protocol.MsgTypeKCP,
		BodyLen: uint16(len(payload)),
		SessID:  t.sessID,
	}
	if err := header.EncodeTo(packet[:protocol.HeaderSize]); err != nil {
		t.server.releasePacketBuffer(packet)
		return
	}

	copy(packet[protocol.HeaderSize:], payload)
	_, _ = t.server.conn.WriteTo(packet, addr)
	t.server.releasePacketBuffer(packet)
}

func newKCPState(server *Server, id uint32, transport *sessionTransport) *kcp.KCP {
	state := kcp.NewKCP(id, func(buf []byte, size int) {
		transport.writeKCP(buf[:size])
	})
	state.NoDelay(
		server.config.KCP.NoDelay,
		server.config.KCP.Interval,
		server.config.KCP.Resend,
		server.config.KCP.NoCongestion,
	)
	state.WndSize(server.config.KCP.SendWindow, server.config.KCP.RecvWindow)
	state.SetMtu(server.config.KCP.MTU)
	return state
}

func ensureScratch(buf []byte, size int) []byte {
	if cap(buf) < size {
		return make([]byte, size)
	}
	return buf[:size]
}

func resetTimer(timer *time.Timer, d time.Duration) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
	timer.Reset(d)
}
