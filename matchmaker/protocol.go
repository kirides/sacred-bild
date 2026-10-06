package main

// Wire format of the matchmaker protocol (docs/UDP_PROTOCOL.md, "Matchmaker"): little-endian integers, IPv4
// addresses as 4 raw bytes in network order.

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
	announcementSize = 0xAE
	nameChars        = (announcementSize - 0x0E) / 2
	gamesPerPage     = 6

	challengeSize  = 32
	registerSize   = 32 + announcementSize
	registeredSize = 32
	unregisterSize = 32
	listSize       = 32
	gamesHeader    = 20
	gameEntrySize  = 4 + 4 + 2 + 2 + announcementSize
	joinSize       = 32
	joinedSize     = 24
	introduceSize  = 24

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

func putAddr(b []byte, a netip.AddrPort) {
	ip := a.Addr().Unmap().As4()
	copy(b, ip[:])
	le.PutUint16(b[4:], a.Port())
}

func getAddr(b []byte) netip.AddrPort {
	return netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[:4])), le.Uint16(b[4:]))
}

// --- requests (host / player -> matchmaker) ---

type registerMsg struct {
	Nonce        uint32
	Flags        uint16
	Cookie       cookie
	Announcement [announcementSize]byte
}

func (m registerMsg) encode() []byte {
	b := header(typeRegister, registerSize)
	le.PutUint32(b[8:], m.Nonce)
	le.PutUint16(b[12:], m.Flags)
	copy(b[16:], m.Cookie[:])
	copy(b[32:], m.Announcement[:])
	return b
}

func decodeRegister(b []byte) (m registerMsg, ok bool) {
	if len(b) < registerSize {
		return m, false
	}
	m.Nonce = le.Uint32(b[8:])
	m.Flags = le.Uint16(b[12:])
	copy(m.Cookie[:], b[16:32])
	copy(m.Announcement[:], b[32:registerSize])
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
	Addr         netip.AddrPort
	Flags        uint16
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
		putAddr(o[4:], e.Addr)
		le.PutUint16(o[10:], e.Flags)
		copy(o[12:], e.Announcement[:])
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
		e := gameEntry{ID: le.Uint32(o), Addr: getAddr(o[4:]), Flags: le.Uint16(o[10:])}
		copy(e.Announcement[:], o[12:12+announcementSize])
		entries = append(entries, e)
	}
	return nonce, page, pageCount, entries, true
}

// encodeJoined: a zero address (netip.AddrPort{}) means "no such game".
func encodeJoined(nonce, gameID uint32, host netip.AddrPort, flags uint16) []byte {
	b := header(typeJoined, joinedSize)
	le.PutUint32(b[8:], nonce)
	le.PutUint32(b[12:], gameID)
	if host.IsValid() {
		putAddr(b[16:], host)
		le.PutUint16(b[22:], flags)
	}
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
