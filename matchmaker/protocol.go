package main

// Wire format of the matchmaker protocol (docs/UDP_PROTOCOL.md, "Matchmaker"): little-endian integers, addresses
// as 18 bytes (16-byte IPv6, IPv4 as ::ffff:a.b.c.d, then the port; all zero = none).

import (
	"encoding/binary"
	"net/netip"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	protocolVersion  = 1
	maxDatagram      = 1200
	cookieSize       = 16
	addrSize         = 18
	announcementSize = 0xAE
	nameChars        = (announcementSize - 0x0E) / 2
	gamesPerPage     = 5

	challengeSize  = 32
	registerSize   = 40 + announcementSize
	registeredSize = 40
	unregisterSize = 32
	listSize       = 32
	gamesHeader    = 20
	gameEntrySize  = 4 + 2 + 2 + 2*addrSize + announcementSize
	joinSize       = 32
	joinedSize     = 56
	introduceSize  = 32

	flagUDP = 1 // REGISTER flags bit 0: the host accepts the UDP transport
)

// Message types: the 4th byte after "SBM".
const (
	typeChallenge  = 'C'
	typeRegister   = 'R'
	typeRegistered = 'A'
	typeUnregister = 'U'
	typeList       = 'L'
	typeGames      = 'G'
	typeJoin       = 'J'
	typeJoined     = 'O'
	typeIntroduce  = 'I'
)

var le = binary.LittleEndian

type cookie [cookieSize]byte

// header returns a zeroed message of `size` bytes with magic and version filled in.
func header(kind byte, size int) []byte {
	b := make([]byte, size)
	copy(b, "SBM")
	b[3] = kind
	le.PutUint32(b[4:], protocolVersion)
	return b
}

// messageType returns the type of a matchmaker datagram of this protocol version, or 0.
func messageType(b []byte) byte {
	if len(b) < 8 || string(b[:3]) != "SBM" || le.Uint32(b[4:]) != protocolVersion {
		return 0
	}
	return b[3]
}

// putAddr writes an 18-byte address; an invalid one (no address) stays all zero.
func putAddr(b []byte, a netip.AddrPort) {
	if !a.IsValid() {
		return
	}
	ip := a.Addr().As16() // IPv4 as ::ffff:a.b.c.d
	copy(b, ip[:])
	le.PutUint16(b[16:], a.Port())
}

func getAddr(b []byte) netip.AddrPort {
	ip := [16]byte(b[:16])
	port := le.Uint16(b[16:])
	if ip == [16]byte{} && port == 0 {
		return netip.AddrPort{}
	}
	return netip.AddrPortFrom(netip.AddrFrom16(ip).Unmap(), port)
}

// --- requests (host / player -> matchmaker) ---

type registerMsg struct {
	Nonce        uint32
	Flags        uint16
	Cookie       cookie
	HostKey      uint64
	Announcement [announcementSize]byte
}

func (m registerMsg) encode() []byte {
	b := header(typeRegister, registerSize)
	le.PutUint32(b[8:], m.Nonce)
	le.PutUint16(b[12:], m.Flags)
	copy(b[16:], m.Cookie[:])
	le.PutUint64(b[32:], m.HostKey)
	copy(b[40:], m.Announcement[:])
	return b
}

func decodeRegister(b []byte) (m registerMsg, ok bool) {
	if len(b) < registerSize {
		return m, false
	}
	m.Nonce = le.Uint32(b[8:])
	m.Flags = le.Uint16(b[12:])
	copy(m.Cookie[:], b[16:32])
	m.HostKey = le.Uint64(b[32:])
	copy(m.Announcement[:], b[40:registerSize])
	return m, true
}

// requestMsg is the 32-byte layout UNREGISTER, LIST and JOIN share: nonce, a u32 argument (gameId; LIST: page in
// the low u16), cookie.
type requestMsg struct {
	Nonce  uint32
	Arg    uint32
	Cookie cookie
}

func (m requestMsg) encode(kind byte) []byte {
	b := header(kind, 32)
	le.PutUint32(b[8:], m.Nonce)
	le.PutUint32(b[12:], m.Arg)
	copy(b[16:], m.Cookie[:])
	return b
}

func decodeRequest(b []byte) (m requestMsg, ok bool) {
	if len(b) < 32 {
		return m, false
	}
	m.Nonce = le.Uint32(b[8:])
	m.Arg = le.Uint32(b[12:])
	copy(m.Cookie[:], b[16:32])
	return m, true
}

// --- replies (matchmaker -> host / player) ---

func encodeChallenge(nonce uint32, c cookie) []byte {
	b := header(typeChallenge, challengeSize)
	le.PutUint32(b[8:], nonce)
	copy(b[16:], c[:])
	return b
}

func encodeRegistered(nonce, gameID, refreshMs uint32, host netip.AddrPort) []byte {
	b := header(typeRegistered, registeredSize)
	le.PutUint32(b[8:], nonce)
	le.PutUint32(b[12:], gameID)
	le.PutUint32(b[16:], refreshMs)
	putAddr(b[20:], host)
	return b
}

type gameEntry struct {
	ID           uint32
	Flags        uint16
	Addr4        netip.AddrPort // invalid: not reachable over IPv4
	Addr6        netip.AddrPort // invalid: not reachable over IPv6
	Announcement [announcementSize]byte
}

func encodeGames(nonce uint32, page, pageCount uint16, entries []gameEntry) []byte {
	b := header(typeGames, gamesHeader+len(entries)*gameEntrySize)
	le.PutUint32(b[8:], nonce)
	le.PutUint16(b[12:], page)
	le.PutUint16(b[14:], pageCount)
	le.PutUint16(b[16:], uint16(len(entries)))
	for i, e := range entries {
		o := b[gamesHeader+i*gameEntrySize:]
		le.PutUint32(o, e.ID)
		le.PutUint16(o[4:], e.Flags)
		putAddr(o[8:], e.Addr4)
		putAddr(o[8+addrSize:], e.Addr6)
		copy(o[8+2*addrSize:], e.Announcement[:])
	}
	return b
}

func decodeGames(b []byte) (nonce uint32, page, pageCount uint16, entries []gameEntry, ok bool) {
	if len(b) < gamesHeader {
		return
	}
	nonce = le.Uint32(b[8:])
	page = le.Uint16(b[12:])
	pageCount = le.Uint16(b[14:])
	count := int(le.Uint16(b[16:]))
	if len(b) < gamesHeader+count*gameEntrySize {
		return
	}
	for i := range count {
		o := b[gamesHeader+i*gameEntrySize:]
		e := gameEntry{ID: le.Uint32(o), Flags: le.Uint16(o[4:]), Addr4: getAddr(o[8:]), Addr6: getAddr(o[8+addrSize:])}
		copy(e.Announcement[:], o[8+2*addrSize:8+2*addrSize+announcementSize])
		entries = append(entries, e)
	}
	return nonce, page, pageCount, entries, true
}

// encodeJoined: no address in either family means "no such game".
func encodeJoined(nonce, gameID uint32, flags uint16, addr4, addr6 netip.AddrPort) []byte {
	b := header(typeJoined, joinedSize)
	le.PutUint32(b[8:], nonce)
	le.PutUint32(b[12:], gameID)
	le.PutUint16(b[16:], flags)
	putAddr(b[20:], addr4)
	putAddr(b[20+addrSize:], addr6)
	return b
}

func encodeIntroduce(gameID uint32, player netip.AddrPort) []byte {
	b := header(typeIntroduce, introduceSize)
	le.PutUint32(b[8:], gameID)
	putAddr(b[12:], player)
	return b
}

// --- Sacred's LAN announcement ---

type announcementInfo struct {
	Version    uint16
	Players    uint8
	MaxPlayers uint8
	Name       string
}

// parseAnnouncement reads what the web UI shows. The name is the host's free text: it is cut at the first NUL,
// and control characters (line breaks, terminal escapes) are replaced so it can't garble logs.
func parseAnnouncement(a *[announcementSize]byte) announcementInfo {
	info := announcementInfo{
		Version:    le.Uint16(a[0:]),
		Players:    a[0x0C],
		MaxPlayers: a[0x0D],
	}
	if info.MaxPlayers > 0 && info.Players > info.MaxPlayers {
		info.Players = info.MaxPlayers
	}
	units := make([]uint16, 0, nameChars)
	for i := range nameChars {
		u := le.Uint16(a[0x0E+2*i:])
		if u == 0 {
			break
		}
		units = append(units, u)
	}
	info.Name = strings.TrimSpace(strings.Map(func(r rune) rune {
		if unicode.IsControl(r) || r == unicode.ReplacementChar {
			return '?'
		}
		return r
	}, string(utf16.Decode(units))))
	return info
}
