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
var ErrSessionNotFound = errors.New("ukcp: session not found")

type SendReport struct {
	Attempted int
	Sent      int
	Failed    int
}

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

	server := &Server{
		conn:     conn,
		handler:  handler,
		config:   config.withDefaults(),
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

func (s *Server) Session(sessID uint32) *Session {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sessions[sessID]
}

func (s *Server) SendToSess(sessID uint32, payload []byte) error {
	sess := s.Session(sessID)
	if sess == nil {
		return ErrSessionNotFound
	}
	return sess.Send(payload)
}

func (s *Server) SendToMultiSess(sessIDs []uint32, payload []byte) SendReport {
	report := SendReport{}
	if len(sessIDs) <= 8 {
		for i, sessID := range sessIDs {
			duplicate := false
			for j := 0; j < i; j++ {
				if sessIDs[j] == sessID {
					duplicate = true
					break
				}
			}
			if duplicate {
				continue
			}
			report.Attempted++
			if err := s.SendToSess(sessID, payload); err != nil {
				report.Failed++
				continue
			}
			report.Sent++
		}
		return report
	}

	seen := make(map[uint32]struct{}, len(sessIDs))
	for _, sessID := range sessIDs {
		if _, exists := seen[sessID]; exists {
			continue
		}
		seen[sessID] = struct{}{}
		report.Attempted++
		if err := s.SendToSess(sessID, payload); err != nil {
			report.Failed++
			continue
		}
		report.Sent++
	}
	return report
}

func (s *Server) SendToAll(payload []byte) SendReport {
	s.mu.RLock()
	report := SendReport{Attempted: len(s.sessions)}
	for _, sess := range s.sessions {
		if err := sess.Send(payload); err != nil {
			report.Failed++
			continue
		}
		report.Sent++
	}
	s.mu.RUnlock()
	return report
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
		if sess := s.Session(header.SessID); sess != nil {
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

	transport := newSessionTransport(s, sessID, addr)
	pending := &pendingAuth{
		sessID:    sessID,
		transport: transport,
		kcp:       newKCPState(s, sessID, transport),
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
	sess := newSession(s, pending.sessID, pending.transport, pending.kcp)
	s.sessions[pending.sessID] = sess
	s.mu.Unlock()

	if old != nil {
		_ = old.closeWithReason(ErrSessionReplaced)
	}
	s.handler.OnSessionOpen(sess)
	return sess
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
