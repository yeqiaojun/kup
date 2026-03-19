package ukcp

import (
	"bytes"
	"net"
	"sync"
	"testing"
	"time"

	kcp "github.com/xtaci/kcp-go/v5"

	"ukcp/protocol"
)

func TestServerRoutesUDPWithoutDeduplication(t *testing.T) {
	conn := newFakePacketConn()
	defer conn.Close()

	type udpEvent struct {
		sessionID uint32
		seq       uint32
		body      []byte
	}

	udpCh := make(chan udpEvent, 2)
	const sessID uint32 = 1001
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			udpCh <- udpEvent{
				sessionID: sess.ID(),
				seq:       packetSeq,
				body:      append([]byte(nil), payload...),
			}
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10001}
	authClient := newTestKCP(t, sessID, nil)
	for _, seg := range authClient.send(t, []byte("auth")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	waitUntil(t, func() bool { return server.Session(sessID) != nil })

	packet := mustPacket(t, protocol.Header{
		MsgType:   protocol.MsgTypeUDP,
		BodyLen:   3,
		SessID:    sessID,
		PacketSeq: 9,
	}, []byte{0xaa, 0xbb, 0xcc})

	conn.inject(packet, addr)

	got := waitFor(t, udpCh)
	if got.sessionID != sessID || got.seq != 9 || !bytes.Equal(got.body, []byte{0xaa, 0xbb, 0xcc}) {
		t.Fatalf("udp event = %+v, want session=%d seq=9 body=aabbcc", got, sessID)
	}

	conn.inject(packet, addr)
	got = waitFor(t, udpCh)
	if got.sessionID != sessID || got.seq != 9 || !bytes.Equal(got.body, []byte{0xaa, 0xbb, 0xcc}) {
		t.Fatalf("second udp event = %+v, want session=%d seq=9 body=aabbcc", got, sessID)
	}
}

func TestServerRoutesKCPUplink(t *testing.T) {
	conn := newFakePacketConn()
	defer conn.Close()

	kcpCh := make(chan []byte, 1)
	const sessID uint32 = 2002
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			kcpCh <- append([]byte(nil), payload...)
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	client := newTestKCP(t, sessID, nil)
	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10002}
	for _, seg := range client.send(t, []byte("auth")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	waitUntil(t, func() bool { return server.Session(sessID) != nil })
	for _, seg := range client.send(t, []byte("hello over kcp")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}

	got := waitFor(t, kcpCh)
	if !bytes.Equal(got, []byte("hello over kcp")) {
		t.Fatalf("OnKCP payload = %q, want %q", got, "hello over kcp")
	}
}

func TestSessionSendUsesKCPDownlink(t *testing.T) {
	conn := newFakePacketConn()
	defer conn.Close()

	sessionCh := make(chan *Session, 1)
	server, err := Serve(conn, HandlerFuncs{
		OnSessionOpenFunc: func(sess *Session) {
			sessionCh <- sess
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	const sessID uint32 = 3003
	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10003}
	authClient := newTestKCP(t, sessID, nil)
	for _, seg := range authClient.send(t, []byte("auth")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	waitUntil(t, func() bool { return server.Session(sessID) != nil })

	sess := waitFor(t, sessionCh)
	if err := sess.Send([]byte("server push")); err != nil {
		t.Fatalf("Session.Send() error = %v", err)
	}

	client := newTestKCP(t, sessID, nil)
	deadline := time.After(2 * time.Second)
	for {
		select {
		case out := <-conn.writeCh:
			header, body, err := protocol.SplitPacket(out.data)
			if err != nil {
				t.Fatalf("SplitPacket() error = %v", err)
			}
			if header.MsgType != protocol.MsgTypeKCP {
				t.Fatalf("header.MsgType = %d, want %d", header.MsgType, protocol.MsgTypeKCP)
			}

			if rc := client.kcp.Input(body, kcp.IKCP_PACKET_REGULAR, true); rc != 0 {
				t.Fatalf("client kcp input rc = %d, want 0", rc)
			}

			buf := make([]byte, 64)
			n := client.kcp.Recv(buf)
			if n < 0 {
				continue
			}

			if !bytes.Equal(buf[:n], []byte("server push")) {
				t.Fatalf("downlink payload = %q, want %q", buf[:n], "server push")
			}
			return
		case <-deadline:
			t.Fatal("timed out waiting for server push")
		}
	}
}

func TestServerRejectsUnknownUDPAndAuthFailure(t *testing.T) {
	conn := newFakePacketConn()
	defer conn.Close()

	udpCh := make(chan struct{}, 1)
	const sessID uint32 = 4004
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return false
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			udpCh <- struct{}{}
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10004}
	conn.inject(mustPacket(t, protocol.Header{
		MsgType:   protocol.MsgTypeUDP,
		BodyLen:   3,
		SessID:    sessID,
		PacketSeq: 1,
	}, []byte("udp")), addr)

	authClient := newTestKCP(t, sessID, nil)
	for _, seg := range authClient.send(t, []byte("auth-denied")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	conn.inject(mustPacket(t, protocol.Header{
		MsgType:   protocol.MsgTypeUDP,
		BodyLen:   5,
		SessID:    sessID,
		PacketSeq: 2,
	}, []byte("again")), addr)

	select {
	case <-udpCh:
		t.Fatal("unexpected udp delivery for unauthenticated session")
	case <-time.After(150 * time.Millisecond):
	}
	if server.Session(sessID) != nil {
		t.Fatal("expected no active session after auth failure")
	}
}

func TestServerAllowsKCPReauthTakeoverForSameSessID(t *testing.T) {
	conn := newFakePacketConn()
	defer conn.Close()

	const sessID uint32 = 5005
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
	}, Config{
		FastReconnectWindow: 20 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr1 := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10005}
	client1 := newTestKCP(t, sessID, nil)
	for _, seg := range client1.send(t, []byte("auth")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			Flags:   protocol.FlagConnect,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr1)
	}
	waitUntil(t, func() bool { return server.Session(sessID) != nil })
	if err := server.SendToSess(sessID, []byte("first-downlink")); err != nil {
		t.Fatalf("SendToSess(first) error = %v", err)
	}
	time.Sleep(30 * time.Millisecond)

	addr2 := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 10006}
	client2 := newTestKCP(t, sessID, nil)
	for _, seg := range client2.send(t, []byte("auth")) {
		conn.inject(mustPacket(t, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			Flags:   protocol.FlagConnect,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr2)
	}

	waitUntil(t, func() bool {
		sess := server.Session(sessID)
		return sess != nil && sess.RemoteAddr() != nil && sess.RemoteAddr().String() == addr2.String()
	})

	if err := server.SendToSess(sessID, []byte("server-after-reauth")); err != nil {
		t.Fatalf("SendToSess() error = %v", err)
	}

	deadline := time.After(2 * time.Second)
	for {
		select {
		case out := <-conn.writeCh:
			if out.addr.String() != addr2.String() {
				continue
			}

			header, body, err := protocol.SplitPacket(out.data)
			if err != nil {
				t.Fatalf("SplitPacket() error = %v", err)
			}
			if header.MsgType != protocol.MsgTypeKCP {
				t.Fatalf("header.MsgType = %d, want %d", header.MsgType, protocol.MsgTypeKCP)
			}

			if rc := client2.kcp.Input(body, kcp.IKCP_PACKET_REGULAR, true); rc != 0 {
				t.Fatalf("client2 kcp input rc = %d, want 0", rc)
			}

			buf := make([]byte, 64)
			n := client2.kcp.Recv(buf)
			if n < 0 {
				continue
			}
			if got := string(buf[:n]); got != "server-after-reauth" {
				t.Fatalf("downlink payload = %q, want %q", got, "server-after-reauth")
			}
			return
		case <-deadline:
			t.Fatal("timed out waiting for reauth downlink")
		}
	}
}

type testKCP struct {
	kcp *kcp.KCP
	out [][]byte
}

func newTestKCP(t *testing.T, sessID uint32, output func([]byte)) *testKCP {
	t.Helper()

	test := &testKCP{}
	test.kcp = kcp.NewKCP(sessID, func(buf []byte, size int) {
		packet := append([]byte(nil), buf[:size]...)
		test.out = append(test.out, packet)
		if output != nil {
			output(packet)
		}
	})
	test.kcp.NoDelay(1, 10, 2, 1)
	test.kcp.WndSize(128, 128)
	if rc := test.kcp.SetMtu(1200); rc != 0 {
		t.Fatalf("SetMtu() = %d, want 0", rc)
	}
	return test
}

func (test *testKCP) send(t *testing.T, payload []byte) [][]byte {
	t.Helper()

	if rc := test.kcp.Send(payload); rc != 0 {
		t.Fatalf("kcp.Send() = %d, want 0", rc)
	}
	test.kcp.Update()
	out := test.out
	test.out = nil
	return out
}

func mustPacket(t *testing.T, header protocol.Header, body []byte) []byte {
	t.Helper()

	wire, err := header.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}
	return append(wire, body...)
}

type packetRead struct {
	data []byte
	addr net.Addr
}

type packetWrite struct {
	data []byte
	addr net.Addr
}

type fakePacketConn struct {
	readCh    chan packetRead
	writeCh   chan packetWrite
	closeCh   chan struct{}
	closeOnce sync.Once
	local     net.Addr
}

func newFakePacketConn() *fakePacketConn {
	return &fakePacketConn{
		readCh:  make(chan packetRead, 32),
		writeCh: make(chan packetWrite, 32),
		closeCh: make(chan struct{}),
		local:   &net.UDPAddr{IP: net.IPv4zero, Port: 9999},
	}
}

func (c *fakePacketConn) inject(data []byte, addr net.Addr) {
	c.readCh <- packetRead{data: append([]byte(nil), data...), addr: addr}
}

func (c *fakePacketConn) ReadFrom(p []byte) (n int, addr net.Addr, err error) {
	select {
	case <-c.closeCh:
		return 0, nil, net.ErrClosed
	case pkt := <-c.readCh:
		n = copy(p, pkt.data)
		return n, pkt.addr, nil
	}
}

func (c *fakePacketConn) WriteTo(p []byte, addr net.Addr) (n int, err error) {
	select {
	case <-c.closeCh:
		return 0, net.ErrClosed
	default:
	}

	c.writeCh <- packetWrite{
		data: append([]byte(nil), p...),
		addr: addr,
	}
	return len(p), nil
}

func (c *fakePacketConn) Close() error {
	c.closeOnce.Do(func() {
		close(c.closeCh)
	})
	return nil
}

func (c *fakePacketConn) LocalAddr() net.Addr              { return c.local }
func (c *fakePacketConn) SetDeadline(time.Time) error      { return nil }
func (c *fakePacketConn) SetReadDeadline(time.Time) error  { return nil }
func (c *fakePacketConn) SetWriteDeadline(time.Time) error { return nil }

func waitFor[T any](t *testing.T, ch <-chan T) T {
	t.Helper()

	select {
	case v := <-ch:
		return v
	case <-time.After(2 * time.Second):
		var zero T
		t.Fatal("timed out waiting for event")
		return zero
	}
}
