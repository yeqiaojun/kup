package ukcp

import (
	"errors"
	"net"
	"sync"
	"time"

	kcp "github.com/xtaci/kcp-go/v5"

	"ukcp/protocol"
)

var ErrServerClosed = errors.New("ukcp: server closed")

type Server struct {
	conn    net.PacketConn
	handler Handler
	config  Config

	closed    chan struct{}
	closeOnce sync.Once
	wg        sync.WaitGroup

	mu       sync.RWMutex
	sessions map[uint32]*Session
	pending  map[uint32]*pendingAuth

	packetPool sync.Pool
}

type pendingAuth struct {
	sessID    uint32
	transport *sessionTransport
	kcp       *kcp.KCP
	recvBuf   []byte
	mtu       int
}

func Listen(addr string, handler Handler, config Config) (*Server, error) {
	conn, err := net.ListenPacket("udp", addr)
	if err != nil {
		return nil, err
	}
	return Serve(conn, handler, config)
}

func Serve(conn net.PacketConn, handler Handler, config Config) (*Server, error) {
	if conn == nil {
		return nil, errors.New("ukcp: nil packet conn")
	}
	if handler == nil {
		handler = HandlerFuncs{}
	}

	cfg := config.withDefaults()
	server := &Server{
		conn:     conn,
		handler:  handler,
		config:   cfg,
		closed:   make(chan struct{}),
		sessions: make(map[uint32]*Session),
		pending:  make(map[uint32]*pendingAuth),
	}
	server.wg.Add(1)
	go server.readLoop()
	return server, nil
}

func (s *Server) Close() error {
	var err error
	s.closeOnce.Do(func() {
		close(s.closed)
		err = s.conn.Close()
	})
	s.wg.Wait()
	return err
}

func (s *Server) FindSession(sessID uint32) *Session {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sessions[sessID]
}

func (s *Server) CloseSession(sessID uint32, reason string) bool {
	s.mu.Lock()
	sess := s.sessions[sessID]
	if sess == nil {
		s.mu.Unlock()
		return false
	}
	delete(s.sessions, sessID)
	s.mu.Unlock()

	_ = sess.closeWithReason(closeReasonError(reason, ErrSessionClosed))
	return true
}

func (s *Server) SetMtu(mtu int) bool {
	if kcpMtuFromTransportMtu(mtu) < kcpMinMtu {
		return false
	}

	s.mu.Lock()
	s.config.KCP.MTU = mtu
	s.mu.Unlock()
	return true
}

func (s *Server) SendKcpToSess(sessID uint32, payload []byte) bool {
	sess := s.FindSession(sessID)
	return sess != nil && sess.SendKcp(payload) == nil
}

func (s *Server) SendKcpToMultiSess(sessIDs []uint32, payload []byte) bool {
	return s.sendToSessionIDs(sessIDs, func(sess *Session) bool {
		return sess.SendKcp(payload) == nil
	})
}

func (s *Server) SendKcpToAll(payload []byte) bool {
	return s.sendToSessions(s.snapshotSessions(), func(sess *Session) bool {
		return sess.SendKcp(payload) == nil
	})
}

func (s *Server) SendUdpToSess(sessID uint32, packetSeq uint32, payload []byte) bool {
	sess := s.FindSession(sessID)
	return sess != nil && sess.SendUdp(packetSeq, payload) == nil
}

func (s *Server) SendUdpToMultiSess(sessIDs []uint32, packetSeq uint32, payload []byte) bool {
	return s.sendToSessionIDs(sessIDs, func(sess *Session) bool {
		return sess.SendUdp(packetSeq, payload) == nil
	})
}

func (s *Server) SendUdpToAll(packetSeq uint32, payload []byte) bool {
	return s.sendToSessions(s.snapshotSessions(), func(sess *Session) bool {
		return sess.SendUdp(packetSeq, payload) == nil
	})
}

func (s *Server) readLoop() {
	defer s.wg.Done()

	buf := make([]byte, s.config.PacketSize)
	for {
		n, addr, err := s.conn.ReadFrom(buf)
		if err != nil {
			if s.isClosed() {
				return
			}
			continue
		}

		header, body, err := protocol.SplitPacket(buf[:n])
		if err != nil {
			continue
		}

		now := time.Now()
		if sess := s.FindSession(header.SessID); sess != nil {
			if !sameAddr(sess.RemoteAddr(), addr) {
				if header.MsgType == protocol.MsgTypeKCP && header.Flags&protocol.FlagConnect != 0 {
					s.handlePendingKCP(header.SessID, body, addr)
				} else if header.MsgType == protocol.MsgTypeKCP && sess.withinFastReconnectWindow(now) {
					payload := append([]byte(nil), body...)
					sess.handleInbound(inboundPacket{
						msgType:   header.MsgType,
						packetSeq: header.PacketSeq,
						payload:   payload,
						addr:      addr,
					})
				}
				continue
			}

			payload := append([]byte(nil), body...)
			sess.handleInbound(inboundPacket{
				msgType:   header.MsgType,
				packetSeq: header.PacketSeq,
				payload:   payload,
				addr:      addr,
			})
			continue
		}

		if header.MsgType == protocol.MsgTypeUDP {
			continue
		}
		s.handlePendingKCP(header.SessID, body, addr)
	}
}

func (s *Server) removeSession(sessID uint32, session *Session) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if current := s.sessions[sessID]; current == session {
		delete(s.sessions, sessID)
	}
}

func (s *Server) isClosed() bool {
	select {
	case <-s.closed:
		return true
	default:
		return false
	}
}

func (s *Server) handlePendingKCP(sessID uint32, payload []byte, addr net.Addr) {
	pending := s.getOrCreatePending(sessID, addr)
	pending.transport.setAddr(addr)
	if rc := pending.kcp.Input(payload, kcp.IKCP_PACKET_REGULAR, s.config.KCP.AckNoDelay); rc != 0 {
		s.removePending(sessID, pending)
		return
	}
	pending.kcp.Update()

	for {
		size := pending.kcp.PeekSize()
		if size < 0 {
			return
		}

		buf := ensureScratch(pending.recvBuf, size)
		pending.recvBuf = buf
		n := pending.kcp.Recv(buf)
		if n < 0 {
			return
		}
		authPayload := buf[:n]
		if !s.handler.Auth(sessID, addr, authPayload) {
			s.removePending(sessID, pending)
			return
		}

		session := s.activatePending(pending)
		session.kickDrain()
		return
	}
}

func (s *Server) getOrCreatePending(sessID uint32, addr net.Addr) *pendingAuth {
	s.mu.Lock()
	defer s.mu.Unlock()
	if pending := s.pending[sessID]; pending != nil {
		if sameAddr(pending.transport.addr(), addr) {
			return pending
		}
	}

	mtu := s.config.KCP.MTU
	transport := newSessionTransport(s, sessID, addr)
	pending := &pendingAuth{
		sessID:    sessID,
		transport: transport,
		kcp:       newKCPState(s.config, sessID, transport, mtu),
		mtu:       mtu,
	}
	s.pending[sessID] = pending
	return pending
}

func (s *Server) removePending(sessID uint32, pending *pendingAuth) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if current := s.pending[sessID]; current == pending {
		delete(s.pending, sessID)
	}
}

func (s *Server) activatePending(pending *pendingAuth) *Session {
	s.mu.Lock()
	delete(s.pending, pending.sessID)
	old := s.sessions[pending.sessID]
	sess := newSession(s, pending.sessID, pending.transport, pending.kcp, pending.mtu)
	s.sessions[pending.sessID] = sess
	s.mu.Unlock()

	if old != nil {
		_ = old.closeWithReason(ErrSessionReplaced)
	}
	s.handler.OnSessionOpen(sess)
	return sess
}

func (s *Server) sendToSessionIDs(sessIDs []uint32, send func(*Session) bool) bool {
	allSent := true
	if len(sessIDs) <= 32 {
		unique := make([]uint32, 0, len(sessIDs))
		for _, sessID := range sessIDs {
			duplicate := false
			for _, seen := range unique {
				if seen == sessID {
					duplicate = true
					break
				}
			}
			if duplicate {
				continue
			}
			unique = append(unique, sessID)
			sess := s.FindSession(sessID)
			sent := sess != nil && send(sess)
			allSent = allSent && sent
		}
		return allSent
	}

	seen := make(map[uint32]struct{}, len(sessIDs))
	for _, sessID := range sessIDs {
		if _, exists := seen[sessID]; exists {
			continue
		}
		seen[sessID] = struct{}{}
		sess := s.FindSession(sessID)
		sent := sess != nil && send(sess)
		allSent = allSent && sent
	}
	return allSent
}

func (s *Server) snapshotSessions() []*Session {
	s.mu.RLock()
	defer s.mu.RUnlock()

	sessions := make([]*Session, 0, len(s.sessions))
	for _, sess := range s.sessions {
		sessions = append(sessions, sess)
	}
	return sessions
}

func (s *Server) sendToSessions(sessions []*Session, send func(*Session) bool) bool {
	allSent := true
	for _, sess := range sessions {
		allSent = allSent && send(sess)
	}
	return allSent
}

func sameAddr(a, b net.Addr) bool {
	if a == nil || b == nil {
		return false
	}
	return a.String() == b.String()
}

func (s *Server) acquirePacketBuffer(size int) []byte {
	if size <= s.config.PacketSize {
		if buf, ok := s.packetPool.Get().([]byte); ok && cap(buf) >= size {
			return buf[:size]
		}
		return make([]byte, size, s.config.PacketSize)
	}
	return make([]byte, size)
}

func (s *Server) releasePacketBuffer(buf []byte) {
	if cap(buf) < s.config.PacketSize {
		return
	}
	s.packetPool.Put(buf[:s.config.PacketSize])
}
