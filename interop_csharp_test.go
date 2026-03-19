package ukcp

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"
)

const interopTestAddr = "127.0.0.1:29030"

func TestCSharpClientInterop100(t *testing.T) {
	conn, err := net.ListenPacket("udp", interopTestAddr)
	if err != nil {
		t.Fatalf("ListenPacket() error = %v", err)
	}
	defer conn.Close()

	var mu sync.Mutex
	counts := make(map[string]int)
	server, err := Serve(conn, HandlerFuncs{
		AuthFunc: func(sessID uint32, addr net.Addr, payload []byte) bool {
			return bytes.Equal(payload, []byte("auth"))
		},
		OnUDPFunc: func(sess *Session, packetSeq uint32, payload []byte) {
			text := string(payload)
			t.Logf("server udp %s", text)
			mu.Lock()
			counts[text]++
			mu.Unlock()
			if err := sess.Send(payload); err != nil {
				t.Errorf("udp echo send failed: %v", err)
			}
		},
		OnKCPFunc: func(sess *Session, payload []byte) {
			text := string(payload)
			t.Logf("server kcp %s", text)
			mu.Lock()
			counts[text]++
			mu.Unlock()
			if err := sess.Send(payload); err != nil {
				t.Errorf("kcp echo send failed: %v", err)
			}
		},
	}, Config{})
	if err != nil {
		t.Fatalf("Serve() error = %v", err)
	}
	defer server.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	cmd := exec.CommandContext(
		ctx,
		"dotnet", "run", "--project", "csharp/UkcpSharp.Console", "--",
		"--host", "127.0.0.1",
		"--port", "29030",
		"--sess", "9901",
		"--count", "25",
	)
	cmd.Dir = "D:\\tkcp\\ukcp"
	output, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		t.Fatalf("dotnet client timed out\n%s", string(output))
	}
	if err != nil {
		t.Fatalf("dotnet client failed: %v\n%s", err, string(output))
	}

	outText := string(output)
	if !strings.Contains(outText, "RESULT sent_kcp=25 sent_udp=75 received=100 expected=100") {
		t.Fatalf("unexpected client output:\n%s", outText)
	}

	mu.Lock()
	defer mu.Unlock()
	if len(counts) != 25 {
		t.Fatalf("server received %d unique payloads, want 25", len(counts))
	}
	for i := 1; i <= 25; i++ {
		key := fmt.Sprintf("%d", i)
		if counts[key] != 4 {
			t.Fatalf("server count for %q = %d, want 4", key, counts[key])
		}
	}
}
