package ukcp

import "net"

type Handler interface {
	Auth(uint32, net.Addr, []byte) bool
	OnSessionOpen(*Session)
	OnUDP(*Session, uint32, []byte)
	OnKCP(*Session, []byte)
	OnSessionClose(*Session, error)
}

type HandlerFuncs struct {
	AuthFunc           func(uint32, net.Addr, []byte) bool
	OnSessionOpenFunc  func(*Session)
	OnUDPFunc          func(*Session, uint32, []byte)
	OnKCPFunc          func(*Session, []byte)
	OnSessionCloseFunc func(*Session, error)
}

func (h HandlerFuncs) Auth(sessID uint32, addr net.Addr, payload []byte) bool {
	if h.AuthFunc != nil {
		return h.AuthFunc(sessID, addr, payload)
	}
	return true
}

func (h HandlerFuncs) OnSessionOpen(sess *Session) {
	if h.OnSessionOpenFunc != nil {
		h.OnSessionOpenFunc(sess)
	}
}

func (h HandlerFuncs) OnUDP(sess *Session, packetSeq uint32, payload []byte) {
	if h.OnUDPFunc != nil {
		h.OnUDPFunc(sess, packetSeq, payload)
	}
}

func (h HandlerFuncs) OnKCP(sess *Session, payload []byte) {
	if h.OnKCPFunc != nil {
		h.OnKCPFunc(sess, payload)
	}
}

func (h HandlerFuncs) OnSessionClose(sess *Session, err error) {
	if h.OnSessionCloseFunc != nil {
		h.OnSessionCloseFunc(sess, err)
	}
}
