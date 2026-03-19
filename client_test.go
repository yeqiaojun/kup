package ukcp

import (
	"bytes"
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

const clientTestAddr = "127.0.0.1:29000"
const serverSendTestAddr = "127.0.0.1:29002"

func TestClientEndToEndUDPAndKCP(t *testing.T) {
	conn, err := net.ListenPacket("udp", clientTestAddr)
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	var udpHits atomic.Int32
	serverKCP := make(chan []byte, 1)
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(sessID uint32, addr net.Addr, payload []byte) bool {
			return bytes.Equal(payload, []byte("auth"))
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			udpHits.Add(1)
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			serverKCP <- append([]byte(nil), payload...)
			if err := sess.Send([]byte("pong")); err != nil {
				t.Errorf("sess.Send() error = %v", err)
			}
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	client, err := Dial(clientTestAddr, 7788, Config{})
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	if err := client.SendUDP(10, []byte("ignored"), 2); err != nil {
		t.Fatalf("SendUDP() pre-auth error = %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	if got := udpHits.Load(); got != 0 {
		t.Fatalf("pre-auth udp hits = %d, want 0", got)
	}

	if err := client.SendAuth([]byte("auth")); err != nil {
		t.Fatalf("SendAuth() error = %v", err)
	}
	time.Sleep(50 * time.Millisecond)

	if err := client.SendUDP(11, []byte("swing"), 3); err != nil {
		t.Fatalf("SendUDP() error = %v", err)
	}

	waitUntil(t, func() bool {
		return udpHits.Load() == 3
	})

	if got := udpHits.Load(); got != 3 {
		t.Fatalf("udp hits = %d, want 3", got)
	}

	if err := client.SendKCP([]byte("ping")); err != nil {
		t.Fatalf("SendKCP() error = %v", err)
	}

	serverPayload := waitFor(t, serverKCP)
	if !bytes.Equal(serverPayload, []byte("ping")) {
		t.Fatalf("server kcp payload = %q, want %q", serverPayload, "ping")
	}

	reply := waitFor(t, client.Recv())
	if !bytes.Equal(reply, []byte("pong")) {
		t.Fatalf("client recv = %q, want %q", reply, "pong")
	}
}

func TestServerSendAPIs(t *testing.T) {
	conn, err := net.ListenPacket("udp", serverSendTestAddr)
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(sessID uint32, addr net.Addr, payload []byte) bool {
			return bytes.Equal(payload, []byte("auth"))
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	client1 := mustDialAndAuth(t, serverSendTestAddr, 8101)
	defer client1.Close()
	client2 := mustDialAndAuth(t, serverSendTestAddr, 8102)
	defer client2.Close()
	client3 := mustDialAndAuth(t, serverSendTestAddr, 8103)
	defer client3.Close()

	if err := server.SendToSess(8101, []byte("one")); err != nil {
		t.Fatalf("SendToSess() error = %v", err)
	}
	if got := waitFor(t, client1.Recv()); !bytes.Equal(got, []byte("one")) {
		t.Fatalf("client1 recv = %q, want %q", got, "one")
	}
	assertNoMessage(t, client2.Recv())
	assertNoMessage(t, client3.Recv())

	report := server.SendToMultiSess([]uint32{8101, 8102, 8102, 9999}, []byte("group"))
	if report.Attempted != 3 || report.Sent != 2 || report.Failed != 1 {
		t.Fatalf("SendToMultiSess() report = %+v, want Attempted=3 Sent=2 Failed=1", report)
	}
	if got := waitFor(t, client1.Recv()); !bytes.Equal(got, []byte("group")) {
		t.Fatalf("client1 recv = %q, want %q", got, "group")
	}
	if got := waitFor(t, client2.Recv()); !bytes.Equal(got, []byte("group")) {
		t.Fatalf("client2 recv = %q, want %q", got, "group")
	}
	assertNoMessage(t, client3.Recv())

	allReport := server.SendToAll([]byte("all"))
	if allReport.Attempted != 3 || allReport.Sent != 3 || allReport.Failed != 0 {
		t.Fatalf("SendToAll() report = %+v, want Attempted=3 Sent=3 Failed=0", allReport)
	}
	if got := waitFor(t, client1.Recv()); !bytes.Equal(got, []byte("all")) {
		t.Fatalf("client1 recv = %q, want %q", got, "all")
	}
	if got := waitFor(t, client2.Recv()); !bytes.Equal(got, []byte("all")) {
		t.Fatalf("client2 recv = %q, want %q", got, "all")
	}
	if got := waitFor(t, client3.Recv()); !bytes.Equal(got, []byte("all")) {
		t.Fatalf("client3 recv = %q, want %q", got, "all")
	}
}

func TestClientReconnectReplacesSessionAndContinuesIO(t *testing.T) {
	conn, err := net.ListenPacket("udp", "127.0.0.1:29003")
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	const sessID uint32 = 8201

	var mu sync.Mutex
	openCount := 0
	closeCount := 0
	var lastCloseErr error
	var udpHits atomic.Int32
	kcpCh := make(chan []byte, 1)

	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
		OnSessionOpenFunc: func(sess *Session) {
			mu.Lock()
			openCount++
			mu.Unlock()
		},
		OnSessionCloseFunc: func(sess *Session, err error) {
			mu.Lock()
			closeCount++
			lastCloseErr = err
			mu.Unlock()
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			udpHits.Add(1)
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			kcpCh <- append([]byte(nil), payload...)
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	client1 := mustDialAndAuth(t, "127.0.0.1:29003", sessID)
	client1.Close()

	client2 := mustDialAndAuth(t, "127.0.0.1:29003", sessID)
	defer client2.Close()

	waitUntil(t, func() bool {
		mu.Lock()
		defer mu.Unlock()
		return openCount == 2 && closeCount == 1
	})

	mu.Lock()
	gotCloseErr := lastCloseErr
	mu.Unlock()
	if !errors.Is(gotCloseErr, ErrSessionReplaced) {
		t.Fatalf("OnSessionClose err = %v, want %v", gotCloseErr, ErrSessionReplaced)
	}

	if err := client2.SendKCP([]byte("ping-reconnect")); err != nil {
		t.Fatalf("client2 SendKCP() error = %v", err)
	}
	if got := waitFor(t, kcpCh); !bytes.Equal(got, []byte("ping-reconnect")) {
		t.Fatalf("server kcp payload = %q, want %q", got, "ping-reconnect")
	}

	if err := client2.SendUDP(21, []byte("step"), 3); err != nil {
		t.Fatalf("client2 SendUDP() error = %v", err)
	}
	waitUntil(t, func() bool {
		return udpHits.Load() == 3
	})

	if err := server.SendToSess(sessID, []byte("after-reconnect")); err != nil {
		t.Fatalf("SendToSess() error = %v", err)
	}
	if got := waitFor(t, client2.Recv()); !bytes.Equal(got, []byte("after-reconnect")) {
		t.Fatalf("client2 recv = %q, want %q", got, "after-reconnect")
	}
}

func TestClientFastReconnectContinuesWithoutSessionReplace(t *testing.T) {
	conn, err := net.ListenPacket("udp", "127.0.0.1:29004")
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	const sessID uint32 = 8202

	var mu sync.Mutex
	openCount := 0
	closeCount := 0
	var udpHits atomic.Int32
	kcpCh := make(chan []byte, 2)

	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(gotSessID uint32, addr net.Addr, payload []byte) bool {
			return gotSessID == sessID && bytes.Equal(payload, []byte("auth"))
		},
		OnSessionOpenFunc: func(sess *Session) {
			mu.Lock()
			openCount++
			mu.Unlock()
		},
		OnSessionCloseFunc: func(sess *Session, err error) {
			mu.Lock()
			closeCount++
			mu.Unlock()
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			udpHits.Add(1)
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			kcpCh <- append([]byte(nil), payload...)
			if bytes.Equal(payload, []byte("after-fast-reconnect")) {
				if err := sess.Send([]byte("server-after-fast-reconnect")); err != nil {
					t.Errorf("sess.Send() error = %v", err)
				}
			}
		},
	}, Config{
		FastReconnectWindow: 2 * time.Second,
	})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	client := mustDialAndAuth(t, "127.0.0.1:29004", sessID)
	defer client.Close()

	if err := client.SendKCP([]byte("before-fast-reconnect")); err != nil {
		t.Fatalf("SendKCP(before) error = %v", err)
	}
	if got := waitFor(t, kcpCh); !bytes.Equal(got, []byte("before-fast-reconnect")) {
		t.Fatalf("server kcp payload = %q, want %q", got, "before-fast-reconnect")
	}

	if err := client.Reconnect(); err != nil {
		t.Fatalf("Reconnect() error = %v", err)
	}

	if err := client.SendKCP([]byte("after-fast-reconnect")); err != nil {
		t.Fatalf("SendKCP(after) error = %v", err)
	}
	if got := waitFor(t, kcpCh); !bytes.Equal(got, []byte("after-fast-reconnect")) {
		t.Fatalf("server kcp payload = %q, want %q", got, "after-fast-reconnect")
	}
	if got := waitFor(t, client.Recv()); !bytes.Equal(got, []byte("server-after-fast-reconnect")) {
		t.Fatalf("client recv = %q, want %q", got, "server-after-fast-reconnect")
	}

	if err := client.SendUDP(22, []byte("udp-after-fast-reconnect"), 3); err != nil {
		t.Fatalf("SendUDP() error = %v", err)
	}
	waitUntil(t, func() bool {
		return udpHits.Load() == 3
	})

	mu.Lock()
	gotOpenCount := openCount
	gotCloseCount := closeCount
	mu.Unlock()
	if gotOpenCount != 1 || gotCloseCount != 0 {
		t.Fatalf("session lifecycle open=%d close=%d, want open=1 close=0", gotOpenCount, gotCloseCount)
	}
}

func waitUntil(t *testing.T, fn func() bool) {
	t.Helper()

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if fn() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("condition not met before timeout")
}

func mustDialAndAuth(t *testing.T, addr string, sessID uint32) *Client {
	t.Helper()

	client, err := Dial(addr, sessID, Config{})
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	if err := client.SendAuth([]byte("auth")); err != nil {
		client.Close()
		t.Fatalf("SendAuth() error = %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	return client
}

func assertNoMessage[T any](t *testing.T, ch <-chan T) {
	t.Helper()

	select {
	case got := <-ch:
		t.Fatalf("unexpected message: %+v", got)
	case <-time.After(150 * time.Millisecond):
	}
}
