package main

import (
	"cmp"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"log"
	"net"
	"net/netip"
	"slices"
	"sync"
	"time"
)

const (
	cookieBucket    = 60 * time.Second
	summaryInterval = 10 * time.Minute
	refuseLogEvery  = time.Minute
)

// endpoint is a host's address in one family, from the latest REGISTER over that family.
type endpoint struct {
	addr     netip.AddrPort // invalid: none
	lastSeen time.Time
}

type game struct {
	id           uint32
	hostKey      uint64     // the host's own random key: its identity across both families
	owner        netip.Addr // limit key of the address that created it (MaxGamesPerAddress)
	v4, v6       endpoint
	flags        uint16
	announcement [announcementSize]byte
	info         announcementInfo
	created      time.Time
}

func (g *game) endpoint(v6 bool) *endpoint {
	if v6 {
		return &g.v6
	}
	return &g.v4
}

type bucket struct {
	tokens float64
	last   time.Time
}

type Server struct {
	cfg    Config
	conn4  *net.UDPConn // either may be nil: that family isn't served
	conn6  *net.UDPConn
	secret [32]byte
	now    func() time.Time

	mu        sync.Mutex
	games     map[uint64]*game         // by hostKey
	byID      map[uint32]*game         // by gameId
	byAddr    map[netip.AddrPort]*game // by either of its addresses
	perOwner  map[netip.Addr]int
	limits    map[netip.Addr]*bucket
	requests  uint64
	dropped   uint64
	refusedAt time.Time
}

// NewServer serves the given sockets (nil ones are skipped), each for the family of its local address.
func NewServer(cfg Config, conns ...*net.UDPConn) *Server {
	s := &Server{
		cfg:      cfg,
		now:      time.Now,
		games:    map[uint64]*game{},
		byID:     map[uint32]*game{},
		byAddr:   map[netip.AddrPort]*game{},
		perOwner: map[netip.Addr]int{},
		limits:   map[netip.Addr]*bucket{},
	}
	for _, c := range conns {
		if c == nil {
			continue
		}
		if c.LocalAddr().(*net.UDPAddr).AddrPort().Addr().Unmap().Is4() {
			s.conn4 = c
		} else {
			s.conn6 = c
		}
	}
	if _, err := rand.Read(s.secret[:]); err != nil {
		panic(err)
	}
	return s
}

func is6(a netip.AddrPort) bool { return !a.Addr().Is4() }

func familyName(v6 bool) string {
	if v6 {
		return "IPv6"
	}
	return "IPv4"
}

// limitKey groups addresses for the rate limit and MaxGamesPerAddress: an IPv4 address, or an IPv6 /64 network
// (one subscriber usually has a whole /64, or more).
func limitKey(a netip.Addr) netip.Addr {
	if a.Is4() {
		return a
	}
	p, _ := a.Prefix(64)
	return p.Addr()
}

// where names an address in the log only with LogSensitiveData=1.
func (s *Server) where(a netip.AddrPort) string {
	if !s.cfg.LogSensitiveData {
		return ""
	}
	return " (" + a.String() + ")"
}

// Serve handles datagrams until ctx ends; the caller closes the connections to stop the read loops.
func (s *Server) Serve(ctx context.Context) error {
	go s.housekeeping(ctx)
	var wg sync.WaitGroup
	for _, c := range []*net.UDPConn{s.conn4, s.conn6} {
		if c == nil {
			continue
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			buf := make([]byte, 2048)
			for {
				n, from, err := c.ReadFromUDPAddrPort(buf)
				if err != nil {
					if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
						return
					}
					// ICMP errors from earlier sends surface here on some systems; they don't concern the socket.
					continue
				}
				s.handle(buf[:n], from)
			}
		}()
	}
	wg.Wait()
	return nil
}

func (s *Server) housekeeping(ctx context.Context) {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	summary := s.now()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
		now := s.now()
		s.expire(now)
		if now.Sub(summary) >= summaryInterval {
			summary = now
			s.mu.Lock()
			log.Printf("summary: %d games, %d requests, %d dropped by the rate limit (last %v)",
				len(s.games), s.requests, s.dropped, summaryInterval)
			s.requests, s.dropped = 0, 0
			s.mu.Unlock()
		}
	}
}

// send answers over the socket of the destination's family.
func (s *Server) send(to netip.AddrPort, b []byte) {
	c := s.conn4
	if is6(to) {
		c = s.conn6
	}
	if c != nil {
		_, _ = c.WriteToUDPAddrPort(b, to)
	}
}

// cookie proves that a requester receives at its source address: only then do replies get larger than requests,
// so spoofed requests can't turn this server into an amplifier.
func (s *Server) cookie(from netip.AddrPort, bucket int64) cookie {
	var msg [addrSize + 8]byte
	putAddr(msg[:], from)
	binary.LittleEndian.PutUint64(msg[addrSize:], uint64(bucket))
	mac := hmac.New(sha256.New, s.secret[:])
	mac.Write(msg[:])
	var c cookie
	copy(c[:], mac.Sum(nil))
	return c
}

func (s *Server) currentCookie(from netip.AddrPort, now time.Time) cookie {
	return s.cookie(from, now.Unix()/int64(cookieBucket/time.Second))
}

// validCookie accepts the current and the previous bucket, so a cookie lives 60 to 120 s.
func (s *Server) validCookie(from netip.AddrPort, c cookie, now time.Time) bool {
	b := now.Unix() / int64(cookieBucket/time.Second)
	cur, prev := s.cookie(from, b), s.cookie(from, b-1)
	return hmac.Equal(c[:], cur[:]) || hmac.Equal(c[:], prev[:])
}

// allow is a token bucket per limit key: RequestsPerSecond, bursts of twice that.
func (s *Server) allow(ip netip.Addr, now time.Time) bool {
	key := limitKey(ip)
	rate := s.cfg.RequestsPerSecond
	b := s.limits[key]
	if b == nil {
		b = &bucket{tokens: 2 * rate, last: now}
		s.limits[key] = b
	}
	b.tokens = min(2*rate, b.tokens+now.Sub(b.last).Seconds()*rate)
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

func (s *Server) handle(b []byte, from netip.AddrPort) {
	from = netip.AddrPortFrom(from.Addr().Unmap().WithZone(""), from.Port())
	kind := messageType(b)
	if kind == 0 || !from.IsValid() {
		return
	}
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.allow(from.Addr(), now) {
		s.dropped++
		return
	}
	s.requests++
	switch kind {
	case typeRegister:
		s.onRegister(b, from, now)
	case typeUnregister:
		s.onUnregister(b, from, now)
	case typeList:
		s.onList(b, from, now)
	case typeJoin:
		s.onJoin(b, from, now)
	}
}

func (s *Server) onRegister(b []byte, from netip.AddrPort, now time.Time) {
	m, ok := decodeRegister(b)
	if !ok {
		return
	}
	if !s.validCookie(from, m.Cookie, now) {
		s.send(from, encodeChallenge(m.Nonce, s.currentCookie(from, now)))
		return
	}
	// The address inside is the host's LAN address: players connect to the ones seen here, so it is not passed on.
	clear(m.Announcement[4:8])
	info := parseAnnouncement(&m.Announcement)
	v6 := is6(from)

	// Another game at this very address is gone: its gameserver ended and a new one uses the socket now.
	if old := s.byAddr[from]; old != nil && old.hostKey != m.HostKey {
		s.dropFamily(old, is6(from), "replaced by a new game at its address")
	}

	g := s.games[m.HostKey]
	if g == nil {
		owner := limitKey(from.Addr())
		if len(s.games) >= s.cfg.MaxGames || s.perOwner[owner] >= s.cfg.MaxGamesPerAddress {
			// The host repeats REGISTER every few seconds: log refusals sparingly.
			if now.Sub(s.refusedAt) >= refuseLogEvery {
				s.refusedAt = now
				log.Printf("game '%s' refused: %d games (MaxGames %d, MaxGamesPerAddress %d)%s",
					info.Name, len(s.games), s.cfg.MaxGames, s.cfg.MaxGamesPerAddress, s.where(from))
			}
			return
		}
		g = &game{id: s.newID(), hostKey: m.HostKey, owner: owner, created: now}
		s.games[m.HostKey] = g
		s.byID[g.id] = g
		s.perOwner[owner]++
		log.Printf("game %08x '%s' created over %s: %d/%d players, version %d, udp %v%s",
			g.id, info.Name, familyName(v6), info.Players, info.MaxPlayers, info.Version, m.Flags&flagUDP != 0,
			s.where(from))
	} else if g.info != info {
		log.Printf("game %08x '%s': %d/%d players", g.id, info.Name, info.Players, info.MaxPlayers)
	}

	e := g.endpoint(v6)
	if e.addr != from {
		if e.addr.IsValid() {
			delete(s.byAddr, e.addr) // NAT rebinding, or a new address
		} else if g.created != now {
			log.Printf("game %08x '%s' now reachable over %s%s", g.id, info.Name, familyName(v6), s.where(from))
		}
		e.addr = from
		s.byAddr[from] = g
	}
	e.lastSeen = now
	g.flags = m.Flags
	g.announcement = m.Announcement
	g.info = info
	s.send(from, encodeRegistered(m.Nonce, g.id, s.cfg.RefreshMs, from))
}

func (s *Server) onUnregister(b []byte, from netip.AddrPort, now time.Time) {
	m, ok := decodeRequest(b)
	// No reply either way: the host sends it once while it shuts down, and the game expires anyway.
	if !ok || !s.validCookie(from, m.Cookie, now) {
		return
	}
	if g := s.byID[m.Arg]; g != nil && (g.v4.addr == from || g.v6.addr == from) {
		log.Printf("game %08x '%s' unregistered", g.id, g.info.Name)
		s.remove(g)
	}
}

func (s *Server) onList(b []byte, from netip.AddrPort, now time.Time) {
	m, ok := decodeRequest(b)
	if !ok {
		return
	}
	if !s.validCookie(from, m.Cookie, now) {
		s.send(from, encodeChallenge(m.Nonce, s.currentCookie(from, now)))
		return
	}
	games := s.sorted()
	pageCount := max(1, (len(games)+gamesPerPage-1)/gamesPerPage)
	page := int(uint16(m.Arg))
	var entries []gameEntry
	if start := page * gamesPerPage; start < len(games) {
		for _, g := range games[start:min(start+gamesPerPage, len(games))] {
			entries = append(entries, gameEntry{ID: g.id, Flags: g.flags, Addr4: g.v4.addr, Addr6: g.v6.addr,
				Announcement: g.announcement})
		}
	}
	s.send(from, encodeGames(m.Nonce, uint16(page), uint16(min(pageCount, 0xFFFF)), entries))
}

func (s *Server) onJoin(b []byte, from netip.AddrPort, now time.Time) {
	m, ok := decodeRequest(b)
	if !ok {
		return
	}
	if !s.validCookie(from, m.Cookie, now) {
		s.send(from, encodeChallenge(m.Nonce, s.currentCookie(from, now)))
		return
	}
	g := s.byID[m.Arg]
	if g == nil {
		s.send(from, encodeJoined(m.Nonce, m.Arg, 0, netip.AddrPort{}, netip.AddrPort{}))
		return
	}
	// The host punches towards the player's address in this family while the player starts its handshake towards
	// all of the host's addresses. A player that has both families sends JOIN over both.
	if host := g.endpoint(is6(from)).addr; host.IsValid() {
		s.send(host, encodeIntroduce(g.id, from))
	}
	s.send(from, encodeJoined(m.Nonce, g.id, g.flags, g.v4.addr, g.v6.addr))
	log.Printf("join of game %08x '%s' over %s%s", g.id, g.info.Name, familyName(is6(from)), s.where(from))
}

func (s *Server) newID() uint32 {
	var b [4]byte
	for {
		_, _ = rand.Read(b[:])
		if id := binary.LittleEndian.Uint32(b[:]); id != 0 && s.byID[id] == nil {
			return id
		}
	}
}

// dropFamily forgets a game's address in one family, and the game if it has no address left.
func (s *Server) dropFamily(g *game, v6 bool, why string) {
	e := g.endpoint(v6)
	if !e.addr.IsValid() {
		return
	}
	delete(s.byAddr, e.addr)
	*e = endpoint{}
	if !g.v4.addr.IsValid() && !g.v6.addr.IsValid() {
		log.Printf("game %08x '%s' %s", g.id, g.info.Name, why)
		s.remove(g)
		return
	}
	log.Printf("game %08x '%s' no longer reachable over %s (%s)", g.id, g.info.Name, familyName(v6), why)
}

func (s *Server) remove(g *game) {
	for _, e := range []endpoint{g.v4, g.v6} {
		if e.addr.IsValid() && s.byAddr[e.addr] == g {
			delete(s.byAddr, e.addr)
		}
	}
	delete(s.games, g.hostKey)
	delete(s.byID, g.id)
	if s.perOwner[g.owner]--; s.perOwner[g.owner] <= 0 {
		delete(s.perOwner, g.owner)
	}
}

func (s *Server) expire(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	why := "expired (no REGISTER for " + s.cfg.GameTimeout.String() + ")"
	for _, g := range s.games {
		for _, v6 := range []bool{false, true} {
			if e := g.endpoint(v6); e.addr.IsValid() && now.Sub(e.lastSeen) > s.cfg.GameTimeout {
				s.dropFamily(g, v6, why)
			}
		}
	}
	for key, b := range s.limits {
		if now.Sub(b.last) > time.Minute {
			delete(s.limits, key)
		}
	}
}

// sorted lists the games oldest first, so pages stay stable while games come and go at the end.
func (s *Server) sorted() []*game {
	games := make([]*game, 0, len(s.games))
	for _, g := range s.games {
		games = append(games, g)
	}
	slices.SortFunc(games, func(a, b *game) int {
		if c := a.created.Compare(b.created); c != 0 {
			return c
		}
		return cmp.Compare(a.id, b.id)
	})
	return games
}

// GameView is what the web UI and /api/games show: no addresses.
type GameView struct {
	ID         uint32 `json:"id"`
	Name       string `json:"name"`
	Players    int    `json:"players"`
	MaxPlayers int    `json:"maxPlayers"`
	Version    int    `json:"version"`
	UDP        bool   `json:"udp"`
	IPv4       bool   `json:"ipv4"`
	IPv6       bool   `json:"ipv6"`
	AgeSeconds int64  `json:"ageSeconds"`
}

func (s *Server) Snapshot() []GameView {
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	views := []GameView{}
	for _, g := range s.sorted() {
		views = append(views, GameView{
			ID:         g.id,
			Name:       g.info.Name,
			Players:    int(g.info.Players),
			MaxPlayers: int(g.info.MaxPlayers),
			Version:    int(g.info.Version),
			UDP:        g.flags&flagUDP != 0,
			IPv4:       g.v4.addr.IsValid(),
			IPv6:       g.v6.addr.IsValid(),
			AgeSeconds: int64(now.Sub(g.created).Seconds()),
		})
	}
	return views
}
