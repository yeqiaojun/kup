package main

import (
	"bytes"
	"context"
	"flag"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"

	"ukcp"
)

func main() {
	listenAddr := flag.String("listen", ":9000", "udp listen address")
	flag.Parse()

	logger := log.New(os.Stdout, "[echo] ", log.LstdFlags|log.Lmicroseconds)
	server, err := ukcp.Listen(*listenAddr, newEchoHandler(logger), ukcp.Config{})
	if err != nil {
		logger.Fatalf("listen: %v", err)
	}
	defer server.Close()

	logger.Printf("listening on %s", *listenAddr)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	<-ctx.Done()
	logger.Printf("shutting down")
}

func newEchoHandler(logger *log.Logger) ukcp.Handler {
	return ukcp.HandlerFuncs{
		AuthFunc: func(sessID uint32, addr net.Addr, payload []byte) bool {
			ok := bytes.Equal(payload, []byte("auth"))
			if logger != nil {
				logger.Printf("auth sess=%d remote=%v ok=%t len=%d", sessID, addr, ok, len(payload))
			}
			return ok
		},
		OnSessionOpenFunc: func(sess *ukcp.Session) {
			if logger != nil {
				logger.Printf("session open sess=%d remote=%v", sess.ID(), sess.RemoteAddr())
			}
		},
		OnUDPFunc: func(sess *ukcp.Session, packetSeq uint32, payload []byte) {
			if logger != nil {
				logger.Printf("udp sess=%d seq=%d payload=%q", sess.ID(), packetSeq, string(payload))
			}
			if err := sess.SendKcp(payload); err != nil && logger != nil {
				logger.Printf("udp echo send failed sess=%d err=%v", sess.ID(), err)
			}
		},
		OnKCPFunc: func(sess *ukcp.Session, payload []byte) {
			if logger != nil {
				logger.Printf("kcp sess=%d payload=%q", sess.ID(), string(payload))
			}
			if err := sess.SendKcp(payload); err != nil && logger != nil {
				logger.Printf("kcp echo send failed sess=%d err=%v", sess.ID(), err)
			}
		},
		OnSessionCloseFunc: func(sess *ukcp.Session, err error) {
			if logger != nil {
				logger.Printf("session close sess=%d err=%v", sess.ID(), err)
			}
		},
	}
}
