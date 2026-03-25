package ukcp

import (
	"bytes"
	"net"
	"testing"
	"time"

	kcp "github.com/xtaci/kcp-go/v5"

	"ukcp/protocol"
)

func BenchmarkServerKCPUplink(b *testing.B) {
	conn := newFakePacketConn()
	defer conn.Close()

	received := make(chan struct{}, 1)
	const sessID uint32 = 9101
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			select {
			case received <- struct{}{}:
			default:
			}
		},
	}, Config{})
	if err != nil {
		b.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 19101}
	client := newBenchKCP(b, sessID)
	for _, seg := range client.send([]byte("auth")) {
		conn.inject(mustPacketFromBench(b, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	waitUntilBench(b, func() bool { return server.FindSession(sessID) != nil })

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		for _, seg := range client.send([]byte("payload")) {
			conn.inject(mustPacketFromBench(b, protocol.Header{
				MsgType: protocol.MsgTypeKCP,
				BodyLen: uint16(len(seg)),
				SessID:  sessID,
			}, seg), addr)
		}
		<-received
	}
}

func BenchmarkServerPendingAuthReject(b *testing.B) {
	conn := newFakePacketConn()
	defer conn.Close()

	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return false
		},
	}, Config{})
	if err != nil {
		b.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 19102}
	client := newBenchKCP(b, 9201)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		for _, seg := range client.send([]byte("auth")) {
			server.handlePendingKCP(9201, seg, addr)
		}
	}
}

func BenchmarkServerSessionSend(b *testing.B) {
	conn := newFakePacketConn()
	defer conn.Close()

	const sessID uint32 = 9301
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
	}, Config{})
	if err != nil {
		b.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 19103}
	client := newBenchKCP(b, sessID)
	for _, seg := range client.send([]byte("auth")) {
		conn.inject(mustPacketFromBench(b, protocol.Header{
			MsgType: protocol.MsgTypeKCP,
			BodyLen: uint16(len(seg)),
			SessID:  sessID,
		}, seg), addr)
	}
	waitUntilBench(b, func() bool { return server.FindSession(sessID) != nil })

	payload := []byte("server-push")
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if !server.SendKcpToSess(sessID, payload) {
			b.Fatal("SendKcpToSess() = false, want true")
		}
		<-conn.writeCh
	}
}

type benchKCP struct {
	kcp *kcp.KCP
	out [][]byte
}

func newBenchKCP(b *testing.B, sessID uint32) *benchKCP {
	b.Helper()

	test := &benchKCP{}
	test.kcp = kcp.NewKCP(sessID, func(buf []byte, size int) {
		packet := append([]byte(nil), buf[:size]...)
		test.out = append(test.out, packet)
	})
	test.kcp.NoDelay(1, 10, 2, 1)
	test.kcp.WndSize(128, 128)
	if rc := test.kcp.SetMtu(1200); rc != 0 {
		b.Fatalf("SetMtu() = %d, want 0", rc)
	}
	return test
}

func (test *benchKCP) send(payload []byte) [][]byte {
	if rc := test.kcp.Send(payload); rc != 0 {
		panic(rc)
	}
	test.kcp.Update()
	out := test.out
	test.out = nil
	return out
}

func mustPacketFromBench(b *testing.B, header protocol.Header, body []byte) []byte {
	b.Helper()

	wire, err := header.MarshalBinary()
	if err != nil {
		b.Fatalf("MarshalBinary() error = %v", err)
	}
	return append(wire, body...)
}

func waitUntilBench(b *testing.B, fn func() bool) {
	b.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if fn() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	b.Fatal("condition not met before timeout")
}
