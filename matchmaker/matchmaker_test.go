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
	reg := registerMsg{Nonce: 7, Flags: flagUDP, Cookie: cookie{1, 2, 3}, HostKey: 0x1122334455667788,
		Announcement: testAnnouncement("x", 1, 4)}
	b := reg.encode()
	if len(b) != registerSize || registerSize != 214 || messageType(b) != typeRegister {
		t.Fatalf("register: %d bytes, type %c", len(b), messageType(b))
	}
	if got, ok := decodeRegister(b); !ok || got != reg {
		t.Fatalf("register round trip: %+v", got)
	}

	req := requestMsg{Nonce: 9, Arg: 0xDEADBEEF, Cookie: cookie{4}}
	if got, ok := decodeRequest(req.encode(typeJoin)); !ok || got != req {
		t.Fatalf("request round trip: %+v", got)
	}

	host4 := netip.MustParseAddrPort("203.0.113.5:40000")
	host6 := netip.MustParseAddrPort("[2001:db8::5]:2105")

	// IPv4 goes on the wire as ::ffff:a.b.c.d; all zero is no address.
	var a [addrSize]byte
	putAddr(a[:], host4)
	if string(a[:16]) != "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xcb\x00\x71\x05" || le.Uint16(a[16:]) != 40000 {
		t.Fatalf("IPv4 address: % x", a)
	}
	if getAddr(a[:]) != host4 || getAddr(make([]byte, addrSize)).IsValid() {
		t.Fatal("address round trip")
	}

	entries := []gameEntry{
		{ID: 1, Flags: flagUDP, Addr4: host4, Addr6: host6, Announcement: testAnnouncement("a", 1, 2)},
		{ID: 2, Addr6: host6, Announcement: testAnnouncement("b", 0, 4)},
		{ID: 3, Addr4: host4, Announcement: testAnnouncement("c", 0, 4)},
	}
	g := encodeGames(5, 1, 3, entries)
	if len(g) != gamesHeader+3*gameEntrySize || gameEntrySize != 218 {
		t.Fatalf("games: %d bytes", len(g))
	}
	nonce, page, pageCount, got, ok := decodeGames(g)
	if !ok || nonce != 5 || page != 1 || pageCount != 3 || len(got) != 3 {
		t.Fatalf("games round trip: %v %v %v %v %+v", ok, nonce, page, pageCount, got)
	}
	for i := range entries {
		if got[i] != entries[i] {
			t.Fatalf("entry %d: %+v, want %+v", i, got[i], entries[i])
		}
	}
	if full := gamesHeader + gamesPerPage*gameEntrySize; full > maxDatagram {
		t.Fatalf("a full page is %d bytes", full)
	}

	j := encodeJoined(3, 4, flagUDP, host4, host6)
	if len(j) != joinedSize || le.Uint16(j[16:]) != flagUDP || getAddr(j[20:]) != host4 || getAddr(j[38:]) != host6 {
		t.Fatalf("joined: % x", j)
	}
	if n := encodeJoined(3, 4, 0, netip.AddrPort{}, netip.AddrPort{}); getAddr(n[20:]).IsValid() || getAddr(n[38:]).IsValid() {
		t.Fatalf("joined, no game: % x", n)
	}
	i := encodeIntroduce(4, host6)
	if len(i) != introduceSize || le.Uint32(i[8:]) != 4 || getAddr(i[12:]) != host6 {
		t.Fatalf("introduce: % x", i)
	}
	r := encodeRegistered(1, 2, 5000, host6)
	if len(r) != registeredSize || getAddr(r[20:]) != host6 || le.Uint32(r[16:]) != 5000 {
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
	s := NewServer(defaultConfig())
	t0 := time.Unix(1_000_000_040, 0) // 20 s into a bucket (1_000_000_020 is a multiple of 60)
	for _, from := range []netip.AddrPort{netip.MustParseAddrPort("203.0.113.5:1000"),
		netip.MustParseAddrPort("[2001:db8::5]:1000")} {
		c := s.currentCookie(from, t0)
		for _, tc := range []struct {
			after time.Duration
			valid bool
		}{{0, true}, {39 * time.Second, true}, {41 * time.Second, true}, {99 * time.Second, true}, {101 * time.Second, false}} {
			if got := s.validCookie(from, c, t0.Add(tc.after)); got != tc.valid {
				t.Errorf("%v after %v: valid %v, want %v", from, tc.after, got, tc.valid)
			}
		}
		if s.validCookie(netip.AddrPortFrom(from.Addr(), 1001), c, t0) {
			t.Errorf("%v: cookie valid for another port", from)
		}
	}
}

func TestLimitKey(t *testing.T) {
	a, b := netip.MustParseAddr("2001:db8:1:2::1"), netip.MustParseAddr("2001:db8:1:2:ffff:ffff:ffff:ffff")
	if limitKey(a) != limitKey(b) || limitKey(a) == limitKey(netip.MustParseAddr("2001:db8:1:3::1")) {
		t.Fatal("IPv6 addresses are limited per /64")
	}
	if v4 := netip.MustParseAddr("203.0.113.5"); limitKey(v4) != v4 {
		t.Fatal("IPv4 addresses are limited per address")
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

// newPeer binds a loopback socket in the family of the server address it talks to.
func newPeer(t *testing.T, server *net.UDPAddr) *peer {
	network, ip := "udp4", net.IPv4(127, 0, 0, 1)
	if server.IP.To4() == nil {
		network, ip = "udp6", net.IPv6loopback
	}
	conn, err := net.ListenUDP(network, &net.UDPAddr{IP: ip})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &peer{t, conn, server}
}

func (p *peer) addr() netip.AddrPort {
	a := p.conn.LocalAddr().(*net.UDPAddr).AddrPort()
	return netip.AddrPortFrom(a.Addr().Unmap(), a.Port())
}

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
	b := p.tryRead(2 * time.Second)
	if b == nil {
		p.t.Fatal("no reply")
	}
	return b
}

func (p *peer) tryRead(timeout time.Duration) []byte {
	buf := make([]byte, 2048)
	_ = p.conn.SetReadDeadline(time.Now().Add(timeout))
	n, err := p.conn.Read(buf)
	if err != nil {
		return nil
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

// register runs CHALLENGE -> REGISTER with cookie and returns the REGISTERED reply.
func (p *peer) register(reg registerMsg) []byte {
	p.t.Helper()
	reg.Cookie = challengeCookie(p.t, p.exchange(reg.encode(), false), reg.Nonce)
	r := p.exchange(reg.encode(), true)
	if messageType(r) != typeRegistered || len(r) != registeredSize || le.Uint32(r[8:]) != reg.Nonce {
		p.t.Fatalf("want REGISTERED, got % x", r)
	}
	return r
}

// cookie gets a cookie for further requests.
func (p *peer) cookie() cookie {
	p.t.Helper()
	return challengeCookie(p.t, p.exchange(requestMsg{Nonce: 99}.encode(typeList), false), 99)
}

type testServer struct {
	s     *Server
	clock *fakeClock
	addr4 *net.UDPAddr
	addr6 *net.UDPAddr // nil without IPv6 loopback
}

// startServer serves 127.0.0.1 and, where it can be bound, ::1.
func startServer(t *testing.T, cfg Config) testServer {
	conn4, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	conn6, _ := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	ts := testServer{s: NewServer(cfg, conn4, conn6), clock: &fakeClock{t: time.Unix(1_000_000_000, 0)},
		addr4: conn4.LocalAddr().(*net.UDPAddr)}
	if conn6 != nil {
		ts.addr6 = conn6.LocalAddr().(*net.UDPAddr)
	}
	ts.s.now = ts.clock.now
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = ts.s.Serve(ctx); close(done) }()
	t.Cleanup(func() {
		cancel()
		conn4.Close()
		if conn6 != nil {
			conn6.Close()
		}
		<-done
	})
	return ts
}

func checkWebHasNoAddresses(t *testing.T, s *Server, want string) {
	t.Helper()
	for _, path := range []string{"/api/games", "/"} {
		rec := httptest.NewRecorder()
		newWebHandler(s, "Test").ServeHTTP(rec, httptest.NewRequest("GET", path, nil))
		body := rec.Body.String()
		if !strings.Contains(body, want) || strings.Contains(body, "127.0.0.1") || strings.Contains(body, "::1") {
			t.Fatalf("%s: %s", path, body)
		}
	}
}

func TestEndToEnd(t *testing.T) {
	cfg := defaultConfig()
	ts := startServer(t, cfg)
	host, player := newPeer(t, ts.addr4), newPeer(t, ts.addr4)

	// Host: REGISTER -> CHALLENGE -> REGISTER with cookie -> REGISTERED.
	r := host.register(registerMsg{Nonce: 11, Flags: flagUDP, HostKey: 42, Announcement: testAnnouncement("Ancaria", 1, 4)})
	gameID := le.Uint32(r[12:])
	if gameID == 0 || le.Uint32(r[16:]) != cfg.RefreshMs || getAddr(r[20:]) != host.addr() {
		t.Fatalf("REGISTERED: % x", r)
	}

	// Every request without a cookie gets a CHALLENGE no larger than itself.
	for _, kind := range []byte{typeList, typeJoin} {
		challengeCookie(t, player.exchange(requestMsg{Nonce: 3}.encode(kind), false), 3)
	}

	// Player: LIST sees the game, with the host's address as seen by the matchmaker and the LAN address removed.
	list := requestMsg{Nonce: 21, Cookie: player.cookie()}
	nonce, page, pageCount, entries, ok := decodeGames(player.exchange(list.encode(typeList), true))
	if !ok || nonce != 21 || page != 0 || pageCount != 1 || len(entries) != 1 {
		t.Fatalf("GAMES: %v %d %d %d %+v", ok, nonce, page, pageCount, entries)
	}
	e := entries[0]
	if e.ID != gameID || e.Addr4 != host.addr() || e.Addr6.IsValid() || e.Flags != flagUDP ||
		string(e.Announcement[4:8]) != "\x00\x00\x00\x00" || parseAnnouncement(&e.Announcement).Name != "Ancaria" {
		t.Fatalf("entry: %+v", e)
	}

	// JOIN: INTRODUCE at the host, JOINED at the player.
	join := requestMsg{Nonce: 31, Arg: gameID, Cookie: list.Cookie}
	j := player.exchange(join.encode(typeJoin), true)
	if messageType(j) != typeJoined || le.Uint32(j[8:]) != 31 || le.Uint32(j[12:]) != gameID ||
		le.Uint16(j[16:]) != flagUDP || getAddr(j[20:]) != host.addr() || getAddr(j[38:]).IsValid() {
		t.Fatalf("JOINED: % x", j)
	}
	i := host.read()
	if messageType(i) != typeIntroduce || le.Uint32(i[8:]) != gameID || getAddr(i[12:]) != player.addr() {
		t.Fatalf("INTRODUCE: % x", i)
	}

	// An unknown game: JOINED with no address, nothing to any host.
	join.Arg = gameID + 1
	if j := player.exchange(join.encode(typeJoin), true); getAddr(j[20:]).IsValid() || getAddr(j[38:]).IsValid() {
		t.Fatalf("JOINED for an unknown game: % x", j)
	}

	checkWebHasNoAddresses(t, ts.s, "Ancaria")

	// Expiry: no REGISTER for longer than GameTimeout.
	ts.clock.add(cfg.GameTimeout + time.Second)
	ts.s.expire(ts.clock.now())
	list.Cookie = player.cookie()
	if _, _, _, entries, _ := decodeGames(player.exchange(list.encode(typeList), true)); len(entries) != 0 {
		t.Fatalf("game didn't expire: %+v", entries)
	}
}

func TestDualStack(t *testing.T) {
	cfg := defaultConfig()
	ts := startServer(t, cfg)
	if ts.addr6 == nil {
		t.Skip("no IPv6 loopback")
	}
	host4, host6 := newPeer(t, ts.addr4), newPeer(t, ts.addr6)
	player4, player6 := newPeer(t, ts.addr4), newPeer(t, ts.addr6)

	// The same hostKey over both families: one game with both addresses.
	reg := registerMsg{Nonce: 1, Flags: flagUDP, HostKey: 0xABCDEF, Announcement: testAnnouncement("Dual", 2, 4)}
	r4, r6 := host4.register(reg), host6.register(reg)
	id := le.Uint32(r4[12:])
	if le.Uint32(r6[12:]) != id || getAddr(r4[20:]) != host4.addr() || getAddr(r6[20:]) != host6.addr() {
		t.Fatalf("REGISTERED: % x / % x", r4, r6)
	}
	if v := ts.s.Snapshot(); len(v) != 1 || !v[0].IPv4 || !v[0].IPv6 {
		t.Fatalf("snapshot: %+v", v)
	}

	// LIST over either family shows both addresses.
	for _, p := range []*peer{player4, player6} {
		list := requestMsg{Nonce: 2, Cookie: p.cookie()}
		_, _, _, entries, ok := decodeGames(p.exchange(list.encode(typeList), true))
		if !ok || len(entries) != 1 || entries[0].Addr4 != host4.addr() || entries[0].Addr6 != host6.addr() {
			t.Fatalf("GAMES: %+v", entries)
		}
	}

	// JOIN is introduced to the host in the family it came over.
	for _, tc := range []struct{ player, host, other *peer }{{player6, host6, host4}, {player4, host4, host6}} {
		join := requestMsg{Nonce: 3, Arg: id, Cookie: tc.player.cookie()}
		j := tc.player.exchange(join.encode(typeJoin), true)
		if getAddr(j[20:]) != host4.addr() || getAddr(j[38:]) != host6.addr() {
			t.Fatalf("JOINED: % x", j)
		}
		if i := tc.host.read(); messageType(i) != typeIntroduce || getAddr(i[12:]) != tc.player.addr() {
			t.Fatalf("INTRODUCE: % x", i)
		}
		if b := tc.other.tryRead(100 * time.Millisecond); b != nil {
			t.Fatalf("INTRODUCE in the other family: % x", b)
		}
	}

	checkWebHasNoAddresses(t, ts.s, "Dual")

	// Only IPv4 keeps registering: IPv6 is dropped, the game stays.
	ts.clock.add(cfg.GameTimeout / 2)
	host4.register(reg)
	ts.clock.add(cfg.GameTimeout/2 + time.Second)
	ts.s.expire(ts.clock.now())
	if v := ts.s.Snapshot(); len(v) != 1 || !v[0].IPv4 || v[0].IPv6 {
		t.Fatalf("after IPv6 expired: %+v", v)
	}

	// A game reachable over IPv6 only: JOIN over IPv4 gets JOINED with its IPv6 address, but no INTRODUCE (the host
	// has no IPv4 address to send it to).
	ts.clock.add(cfg.GameTimeout + time.Second)
	host6.register(reg)
	ts.s.expire(ts.clock.now())
	if v := ts.s.Snapshot(); len(v) != 1 || v[0].IPv4 || !v[0].IPv6 {
		t.Fatalf("after IPv4 expired: %+v", v)
	}
	join := requestMsg{Nonce: 4, Arg: id, Cookie: player4.cookie()}
	j := player4.exchange(join.encode(typeJoin), true)
	if getAddr(j[20:]).IsValid() || getAddr(j[38:]) != host6.addr() {
		t.Fatalf("JOINED: % x", j)
	}
	for _, h := range []*peer{host4, host6} {
		if b := h.tryRead(100 * time.Millisecond); b != nil {
			t.Fatalf("unexpected INTRODUCE: % x", b)
		}
	}
}

// handleAt drives the server without sockets (its replies go nowhere): for addresses loopback can't provide.
func handleAt(s *Server, from netip.AddrPort, reg registerMsg) {
	reg.Cookie = s.currentCookie(from, s.now())
	s.handle(reg.encode(), from)
}

func TestLimitsPerNetwork(t *testing.T) {
	cfg := defaultConfig()
	cfg.MaxGamesPerAddress = 1
	s := NewServer(cfg)
	clock := &fakeClock{t: time.Unix(1_000_000_000, 0)}
	s.now = clock.now
	a := testAnnouncement("g", 0, 4)

	// Two hosts in the same /64: the second game is refused; another /64 is fine.
	handleAt(s, netip.MustParseAddrPort("[2001:db8:1:2::1]:2105"), registerMsg{HostKey: 1, Announcement: a})
	handleAt(s, netip.MustParseAddrPort("[2001:db8:1:2::2]:2105"), registerMsg{HostKey: 2, Announcement: a})
	handleAt(s, netip.MustParseAddrPort("[2001:db8:1:3::1]:2105"), registerMsg{HostKey: 3, Announcement: a})
	if n := len(s.Snapshot()); n != 2 {
		t.Fatalf("%d games, want 2", n)
	}

	// The rate limit counts the /64 as one requester.
	cfg.RequestsPerSecond = 2
	s = NewServer(cfg)
	now := time.Unix(1_000_000_000, 0)
	allowed := 0
	for i := range 10 {
		if s.allow(netip.AddrFrom16([16]byte{0x20, 0x01, 0x0d, 0xb8, 15: byte(i)}), now) {
			allowed++
		}
	}
	if allowed != 4 {
		t.Fatalf("burst allowed %d, want 4", allowed)
	}
}

func TestNewGameAtSameAddress(t *testing.T) {
	s := NewServer(defaultConfig())
	clock := &fakeClock{t: time.Unix(1_000_000_000, 0)}
	s.now = clock.now
	from := netip.MustParseAddrPort("203.0.113.5:2105")
	handleAt(s, from, registerMsg{HostKey: 1, Announcement: testAnnouncement("old", 0, 4)})
	// The gameserver restarted: a new hostKey from the same socket replaces the old game right away.
	handleAt(s, from, registerMsg{HostKey: 2, Announcement: testAnnouncement("new", 0, 4)})
	if v := s.Snapshot(); len(v) != 1 || v[0].Name != "new" {
		t.Fatalf("%+v", v)
	}
}

func TestUnregisterAndLimits(t *testing.T) {
	cfg := defaultConfig()
	cfg.MaxGamesPerAddress = 1
	ts := startServer(t, cfg)
	a, b := newPeer(t, ts.addr4), newPeer(t, ts.addr4) // same IP, different ports

	r := a.register(registerMsg{Nonce: 1, HostKey: 1, Announcement: testAnnouncement("g", 0, 4)})
	// A second game from the same IP is refused: no answer.
	reg := registerMsg{Nonce: 1, HostKey: 2, Announcement: testAnnouncement("g", 0, 4)}
	reg.Cookie = challengeCookie(t, b.exchange(reg.encode(), false), 1)
	_, _ = b.conn.WriteToUDP(reg.encode(), b.to)
	if r := b.tryRead(300 * time.Millisecond); r != nil {
		t.Fatalf("second game from the same IP accepted: % x", r)
	}

	// UNREGISTER from another address or with the wrong gameId is ignored; from the host with its id it removes it.
	id := le.Uint32(r[12:])
	for _, tc := range []struct {
		p    *peer
		arg  uint32
		left int
	}{{b, id, 1}, {a, id + 1, 1}, {a, id, 0}} {
		_, _ = tc.p.conn.WriteToUDP(requestMsg{Nonce: 3, Arg: tc.arg, Cookie: tc.p.cookie()}.encode(typeUnregister), tc.p.to)
		time.Sleep(50 * time.Millisecond)
		if got := len(ts.s.Snapshot()); got != tc.left {
			t.Fatalf("UNREGISTER %d: %d games, want %d", tc.arg, got, tc.left)
		}
	}
}

func TestRateLimit(t *testing.T) {
	cfg := defaultConfig()
	cfg.RequestsPerSecond = 2
	s := NewServer(cfg)
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

func TestListenUDP(t *testing.T) {
	conns, err := listenUDP("127.0.0.1:0")
	if err != nil || len(conns) != 1 || conns[0].LocalAddr().(*net.UDPAddr).IP.To4() == nil {
		t.Fatalf("IPv4: %v %v", conns, err)
	}
	conns[0].Close()
	if conns, err := listenUDP("[::1]:0"); err == nil {
		if len(conns) != 1 || conns[0].LocalAddr().(*net.UDPAddr).IP.To4() != nil {
			t.Fatalf("IPv6: %v", conns)
		}
		conns[0].Close()
	}
	if _, err := listenUDP(":notaport"); err == nil {
		t.Fatal("bad port accepted")
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
