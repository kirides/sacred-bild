package main

import (
	"context"
	"encoding/binary"
	"io"
	"log"
	"net"
	"net/http/httptest"
	"net/netip"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf16"
)

func init() { log.SetOutput(io.Discard) }

func testAnnouncement(name string, players, maxPlayers uint8) (a [announcementSize]byte) {
	binary.LittleEndian.PutUint16(a[0:], 1234)
	binary.LittleEndian.PutUint16(a[2:], 2006)
	copy(a[4:8], []byte{192, 168, 1, 10})
	a[0x0C], a[0x0D] = players, maxPlayers
	for i, u := range utf16.Encode([]rune(name)) {
		binary.LittleEndian.PutUint16(a[0x0E+2*i:], u)
	}
	return a
}

func TestRoundTrips(t *testing.T) {
	reg := registerMsg{Nonce: 7, Flags: flagUDP, Cookie: cookie{1, 2, 3}, Announcement: testAnnouncement("x", 1, 4)}
	b := reg.encode()
	if len(b) != registerSize || messageType(b) != typeRegister {
		t.Fatalf("register: %d bytes, type %c", len(b), messageType(b))
	}
	if got, ok := decodeRegister(b); !ok || got != reg {
		t.Fatalf("register round trip: %+v", got)
	}

	req := requestMsg{Nonce: 9, Arg: 0xDEADBEEF, Cookie: cookie{4}}
	if got, ok := decodeRequest(req.encode(typeJoin)); !ok || got != req {
		t.Fatalf("request round trip: %+v", got)
	}

	host := netip.MustParseAddrPort("203.0.113.5:40000")
	entries := []gameEntry{{ID: 1, Addr: host, Flags: flagUDP, Announcement: testAnnouncement("a", 1, 2)},
		{ID: 2, Addr: netip.MustParseAddrPort("198.51.100.1:2105"), Announcement: testAnnouncement("b", 0, 4)}}
	g := encodeGames(5, 1, 3, entries)
	if len(g) != gamesHeader+2*gameEntrySize {
		t.Fatalf("games: %d bytes", len(g))
	}
	nonce, page, pageCount, got, ok := decodeGames(g)
	if !ok || nonce != 5 || page != 1 || pageCount != 3 || len(got) != 2 || got[0] != entries[0] || got[1] != entries[1] {
		t.Fatalf("games round trip: %v %v %v %v %+v", ok, nonce, page, pageCount, got)
	}
	if full := gamesHeader + gamesPerPage*gameEntrySize; full > maxDatagram {
		t.Fatalf("a full page is %d bytes", full)
	}

	j := encodeJoined(3, 4, host, flagUDP)
	if len(j) != joinedSize || getAddr(j[16:]) != host || binary.LittleEndian.Uint16(j[22:]) != flagUDP {
		t.Fatalf("joined: % x", j)
	}
	if n := encodeJoined(3, 4, netip.AddrPort{}, 0); string(n[16:20]) != "\x00\x00\x00\x00" {
		t.Fatalf("joined, no game: % x", n)
	}
	i := encodeIntroduce(4, host)
	if len(i) != introduceSize || binary.LittleEndian.Uint32(i[8:]) != 4 || getAddr(i[12:]) != host {
		t.Fatalf("introduce: % x", i)
	}
	r := encodeRegistered(1, 2, 5000, host)
	if len(r) != registeredSize || getAddr(r[20:]) != host || binary.LittleEndian.Uint32(r[16:]) != 5000 {
		t.Fatalf("registered: % x", r)
	}
}

func TestAnnouncementName(t *testing.T) {
	a := testAnnouncement("Ancaria\x1b[2J\nnight", 9, 4)
	info := parseAnnouncement(&a)
	if info.Name != "Ancaria?[2J?night" || info.Players != 4 || info.MaxPlayers != 4 || info.Version != 1234 {
		t.Fatalf("%+v", info)
	}
	full := testAnnouncement(strings.Repeat("x", nameChars), 1, 4) // no terminating NUL
	if info := parseAnnouncement(&full); len(info.Name) != nameChars {
		t.Fatalf("full-length name: %d chars", len(info.Name))
	}
}

func TestCookieBuckets(t *testing.T) {
	s := NewServer(defaultConfig(), nil)
	from := netip.MustParseAddrPort("203.0.113.5:1000")
	t0 := time.Unix(1_000_000_040, 0) // 20 s into a bucket (1_000_000_020 is a multiple of 60)
	c := s.currentCookie(from, t0)
	for _, tc := range []struct {
		after time.Duration
		valid bool
	}{{0, true}, {39 * time.Second, true}, {41 * time.Second, true}, {99 * time.Second, true}, {101 * time.Second, false}} {
		if got := s.validCookie(from, c, t0.Add(tc.after)); got != tc.valid {
			t.Errorf("after %v: valid %v, want %v", tc.after, got, tc.valid)
		}
	}
	if s.validCookie(netip.MustParseAddrPort("203.0.113.5:1001"), c, t0) {
		t.Error("cookie valid for another port")
	}
}

// fakeClock lets a test move time forward for expiry and cookies.
type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *fakeClock) now() time.Time { c.mu.Lock(); defer c.mu.Unlock(); return c.t }
func (c *fakeClock) add(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

type peer struct {
	t    *testing.T
	conn *net.UDPConn
	to   *net.UDPAddr
}

func newPeer(t *testing.T, server *net.UDPAddr) *peer {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &peer{t, conn, server}
}

func (p *peer) addr() netip.AddrPort { return p.conn.LocalAddr().(*net.UDPAddr).AddrPort() }

// exchange sends a request and returns the reply; it checks that a reply to a request without a valid cookie is
// never larger than the request.
func (p *peer) exchange(req []byte, withCookie bool) []byte {
	p.t.Helper()
	if _, err := p.conn.WriteToUDP(req, p.to); err != nil {
		p.t.Fatal(err)
	}
	reply := p.read()
	if !withCookie && len(reply) > len(req) {
		p.t.Fatalf("%c without cookie: %d-byte reply to a %d-byte request", req[3], len(reply), len(req))
	}
	return reply
}

func (p *peer) read() []byte {
	p.t.Helper()
	buf := make([]byte, 2048)
	_ = p.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := p.conn.Read(buf)
	if err != nil {
		p.t.Fatal(err)
	}
	return buf[:n]
}

func challengeCookie(t *testing.T, b []byte, nonce uint32) (c cookie) {
	t.Helper()
	if messageType(b) != typeChallenge || len(b) != challengeSize || le.Uint32(b[8:]) != nonce {
		t.Fatalf("want CHALLENGE for nonce %d, got % x", nonce, b)
	}
	copy(c[:], b[16:])
	return c
}

func startServer(t *testing.T, cfg Config) (*Server, *fakeClock, *net.UDPAddr) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	s := NewServer(cfg, conn)
	clock := &fakeClock{t: time.Unix(1_000_000_000, 0)}
	s.now = clock.now
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = s.Serve(ctx); close(done) }()
	t.Cleanup(func() { cancel(); conn.Close(); <-done })
	return s, clock, conn.LocalAddr().(*net.UDPAddr)
}

func TestEndToEnd(t *testing.T) {
	cfg := defaultConfig()
	s, clock, addr := startServer(t, cfg)
	host, player := newPeer(t, addr), newPeer(t, addr)

	// Host: REGISTER -> CHALLENGE -> REGISTER with cookie -> REGISTERED.
	reg := registerMsg{Nonce: 11, Flags: flagUDP, Announcement: testAnnouncement("Ancaria", 1, 4)}
	reg.Cookie = challengeCookie(t, host.exchange(reg.encode(), false), 11)
	r := host.exchange(reg.encode(), true)
	if messageType(r) != typeRegistered || len(r) != registeredSize || le.Uint32(r[8:]) != 11 {
		t.Fatalf("want REGISTERED, got % x", r)
	}
	gameID := le.Uint32(r[12:])
	if gameID == 0 || le.Uint32(r[16:]) != cfg.RefreshMs || getAddr(r[20:]) != host.addr() {
		t.Fatalf("REGISTERED: % x", r)
	}

	// Every request without a cookie gets a CHALLENGE no larger than itself.
	for _, kind := range []byte{typeList, typeJoin} {
		challengeCookie(t, player.exchange(requestMsg{Nonce: 3}.encode(kind), false), 3)
	}

	// Player: LIST sees the game, with the host's address as seen by the matchmaker and the LAN address removed.
	list := requestMsg{Nonce: 21}
	list.Cookie = challengeCookie(t, player.exchange(list.encode(typeList), false), 21)
	nonce, page, pageCount, entries, ok := decodeGames(player.exchange(list.encode(typeList), true))
	if !ok || nonce != 21 || page != 0 || pageCount != 1 || len(entries) != 1 {
		t.Fatalf("GAMES: %v %d %d %d %+v", ok, nonce, page, pageCount, entries)
	}
	e := entries[0]
	if e.ID != gameID || e.Addr != host.addr() || e.Flags != flagUDP || string(e.Announcement[4:8]) != "\x00\x00\x00\x00" ||
		parseAnnouncement(&e.Announcement).Name != "Ancaria" {
		t.Fatalf("entry: %+v", e)
	}

	// JOIN: INTRODUCE at the host, JOINED at the player.
	join := requestMsg{Nonce: 31, Arg: gameID, Cookie: list.Cookie}
	j := player.exchange(join.encode(typeJoin), true)
	if messageType(j) != typeJoined || le.Uint32(j[8:]) != 31 || le.Uint32(j[12:]) != gameID ||
		getAddr(j[16:]) != host.addr() || le.Uint16(j[22:]) != flagUDP {
		t.Fatalf("JOINED: % x", j)
	}
	i := host.read()
	if messageType(i) != typeIntroduce || le.Uint32(i[8:]) != gameID || getAddr(i[12:]) != player.addr() {
		t.Fatalf("INTRODUCE: % x", i)
	}

	// An unknown game: JOINED with no address, nothing to any host.
	join.Arg = gameID + 1
	if j := player.exchange(join.encode(typeJoin), true); le.Uint32(j[16:]) != 0 {
		t.Fatalf("JOINED for an unknown game: % x", j)
	}

	// The web views carry no addresses.
	rec := httptest.NewRecorder()
	newWebHandler(s, "Test").ServeHTTP(rec, httptest.NewRequest("GET", "/api/games", nil))
	body := rec.Body.String()
	if !strings.Contains(body, `"name":"Ancaria"`) || strings.Contains(body, "127.0.0.1") {
		t.Fatalf("/api/games: %s", body)
	}
	rec = httptest.NewRecorder()
	newWebHandler(s, "Test").ServeHTTP(rec, httptest.NewRequest("GET", "/", nil))
	if body := rec.Body.String(); !strings.Contains(body, "Ancaria") || strings.Contains(body, "127.0.0.1") {
		t.Fatalf("/: %s", body)
	}

	// Expiry: no REGISTER for longer than GameTimeout.
	clock.add(cfg.GameTimeout + time.Second)
	s.expire(clock.now())
	if _, _, _, entries, _ := decodeGames(player.exchange(list.encode(typeList), true)); len(entries) != 0 {
		t.Fatalf("game didn't expire: %+v", entries)
	}
}

func TestUnregisterAndLimits(t *testing.T) {
	cfg := defaultConfig()
	cfg.MaxGamesPerAddress = 1
	s, _, addr := startServer(t, cfg)
	a, b := newPeer(t, addr), newPeer(t, addr) // same IP, different ports

	register := func(p *peer) []byte {
		reg := registerMsg{Nonce: 1, Announcement: testAnnouncement("g", 0, 4)}
		reg.Cookie = challengeCookie(t, p.exchange(reg.encode(), false), 1)
		_, _ = p.conn.WriteToUDP(reg.encode(), p.to)
		_ = p.conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		buf := make([]byte, 64)
		n, err := p.conn.Read(buf)
		if err != nil {
			return nil
		}
		return buf[:n]
	}
	r := register(a)
	if messageType(r) != typeRegistered {
		t.Fatalf("first game: % x", r)
	}
	if r := register(b); r != nil {
		t.Fatalf("second game from the same IP accepted: % x", r)
	}

	// UNREGISTER with the wrong gameId is ignored, with the right one removes the game.
	c := challengeCookie(t, a.exchange(requestMsg{Nonce: 2}.encode(typeList), false), 2)
	id := le.Uint32(r[12:])
	for _, arg := range []uint32{id + 1, id} {
		_, _ = a.conn.WriteToUDP(requestMsg{Nonce: 3, Arg: arg, Cookie: c}.encode(typeUnregister), a.to)
		time.Sleep(50 * time.Millisecond)
		if got, want := len(s.Snapshot()), map[bool]int{true: 0, false: 1}[arg == id]; got != want {
			t.Fatalf("UNREGISTER %d: %d games, want %d", arg, got, want)
		}
	}
}

func TestRateLimit(t *testing.T) {
	cfg := defaultConfig()
	cfg.RequestsPerSecond = 2
	s := NewServer(cfg, nil)
	ip := netip.MustParseAddr("203.0.113.5")
	now := time.Unix(1_000_000_000, 0)
	allowed := 0
	for range 10 {
		if s.allow(ip, now) {
			allowed++
		}
	}
	if allowed != 4 || !s.allow(ip, now.Add(time.Second)) {
		t.Fatalf("burst allowed %d, want 4", allowed)
	}
}

func TestConfig(t *testing.T) {
	cfg, err := loadConfig([]string{"-config", writeTemp(t, "[Matchmaker]\n; comment\nListen = :3000\nLogSensitiveData=1\nTitle=My games\n"),
		"-webListen=", "-gameTimeout", "30"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Listen != ":3000" || cfg.WebListen != "" || !cfg.LogSensitiveData || cfg.GameTimeout != 30*time.Second ||
		cfg.Title != "My games" || cfg.MaxGames != 1000 {
		t.Fatalf("%+v", cfg)
	}
	if _, err := loadConfig([]string{"-config", writeTemp(t, "Bogus=1\n")}); err == nil {
		t.Fatal("unknown key accepted")
	}
}

func writeTemp(t *testing.T, content string) string {
	path := t.TempDir() + "/matchmaker.ini"
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}
