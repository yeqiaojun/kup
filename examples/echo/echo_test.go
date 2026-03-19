package main

import (
	"bytes"
	"io"
	"log"
	"net"
	"testing"
	"time"

	"ukcp"
)

const echoTestAddr = "127.0.0.1:29001"

func TestEchoServerRepeatingUDPReturnsSingleKCPEcho(t *testing.T) {
	server, addr := startEchoServer(t)
	defer server.Close()

	client, err := ukcp.Dial(addr, 9001, ukcp.Config{})
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	if err := client.SendAuth([]byte("auth")); err != nil {
		t.Fatalf("SendAuth() error = %v", err)
	}
	time.Sleep(50 * time.Millisecond)

	if err := client.SendUDP(7, []byte("udp-echo"), 3); err != nil {
		t.Fatalf("SendUDP() error = %v", err)
	}

	reply := waitFor(t, client.Recv())
	if !bytes.Equal(reply, []byte("udp-echo")) {
		t.Fatalf("udp echo = %q, want %q", reply, "udp-echo")
	}

	for i := 0; i < 2; i++ {
		extra := waitFor(t, client.Recv())
		if !bytes.Equal(extra, []byte("udp-echo")) {
			t.Fatalf("extra udp echo = %q, want %q", extra, "udp-echo")
		}
	}
}

func TestEchoServerKCPEcho(t *testing.T) {
	server, addr := startEchoServer(t)
	defer server.Close()

	client, err := ukcp.Dial(addr, 9002, ukcp.Config{})
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	if err := client.SendAuth([]byte("auth")); err != nil {
		t.Fatalf("SendAuth() error = %v", err)
	}
	time.Sleep(50 * time.Millisecond)

	if err := client.SendKCP([]byte("kcp-echo")); err != nil {
		t.Fatalf("SendKCP() error = %v", err)
	}

	reply := waitFor(t, client.Recv())
	if !bytes.Equal(reply, []byte("kcp-echo")) {
		t.Fatalf("kcp echo = %q, want %q", reply, "kcp-echo")
	}
}

func startEchoServer(t *testing.T) (*ukcp.Server, string) {
	t.Helper()

	conn, err := net.ListenPacket("udp", echoTestAddr)
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}

	server, err := ukcp.Serve(conn, newEchoHandler(log.New(io.Discard, "", 0)), ukcp.Config{})
	if err != nil {
		conn.Close()
		t.Fatalf("Serve() error = %v", err)
	}

	return server, echoTestAddr
}

func waitFor[T any](t *testing.T, ch <-chan T) T {
	t.Helper()

	select {
	case v := <-ch:
		return v
	case <-time.After(2 * time.Second):
		var zero T
		t.Fatal("timed out waiting for echo")
		return zero
	}
}
