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

type game struct {
	id           uint32
	addr         netip.AddrPort // the host's endpoint as seen here: its identity, and where players connect to
	flags        uint16
	announcement [announcementSize]byte
	info         announcementInfo
	created      time.Time
	lastSeen     time.Time
}

type bucket struct {
	tokens float64
	last   time.Time
}

type Server struct {
	cfg    Config
	conn   *net.UDPConn
	secret [32]byte
	now    func() time.Time

	mu        sync.Mutex
	games     map[netip.AddrPort]*game
	byID      map[uint32]*game
	perIP     map[netip.Addr]int
	limits    map[netip.Addr]*bucket
	requests  uint64
	dropped   uint64
	refusedAt time.Time
}

func NewServer(cfg Config, conn *net.UDPConn) *Server {
	s := &Server{
		cfg:    cfg,
		conn:   conn,
		now:    time.Now,
		games:  map[netip.AddrPort]*game{},
		byID:   map[uint32]*game{},
		perIP:  map[netip.Addr]int{},
		limits: map[netip.Addr]*bucket{},
	}
	if _, err := rand.Read(s.secret[:]); err != nil {
		panic(err)
	}
	return s
}

// where names an address in the log only with LogSensitiveData=1.
func (s *Server) where(a netip.AddrPort) string {
	if !s.cfg.LogSensitiveData {
		return ""
	}
	return " (" + a.String() + ")"
}

// Serve handles datagrams until ctx ends; the caller closes the connection to stop the read loop.
func (s *Server) Serve(ctx context.Context) error {
	go s.housekeeping(ctx)
	buf := make([]byte, 2048)
	for {
		n, from, err := s.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return nil
			}
			// ICMP errors from earlier sends surface here on some systems; they don't concern the socket.
			continue
		}
		s.handle(buf[:n], from)
	}
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

func (s *Server) send(to netip.AddrPort, b []byte) {
	_, _ = s.conn.WriteToUDPAddrPort(b, to)
}

// cookie proves that a requester receives at its source address: only then do replies get larger than requests,
// so spoofed requests can't turn this server into an amplifier.
func (s *Server) cookie(from netip.AddrPort, bucket int64) cookie {
	var msg [14]byte
	putAddr(msg[:], from)
	binary.LittleEndian.PutUint64(msg[6:], uint64(bucket))
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

// allow is a per-IP token bucket: RequestsPerSecond, bursts of twice that.
func (s *Server) allow(ip netip.Addr, now time.Time) bool {
	rate := s.cfg.RequestsPerSecond
	b := s.limits[ip]
	if b == nil {
		b = &bucket{tokens: 2 * rate, last: now}
		s.limits[ip] = b
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
	from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
	kind := messageType(b)
	if kind == 0 || !from.Addr().Is4() {
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
	// The address inside is the host's LAN address: players connect to the one seen here, so it is not passed on.
	clear(m.Announcement[4:8])
	info := parseAnnouncement(&m.Announcement)

	g := s.games[from]
	if g == nil {
		if len(s.games) >= s.cfg.MaxGames || s.perIP[from.Addr()] >= s.cfg.MaxGamesPerAddress {
			// The host repeats REGISTER every few seconds: log refusals sparingly.
			if now.Sub(s.refusedAt) >= refuseLogEvery {
				s.refusedAt = now
				log.Printf("game '%s' refused: %d games (MaxGames %d, MaxGamesPerAddress %d)%s",
					info.Name, len(s.games), s.cfg.MaxGames, s.cfg.MaxGamesPerAddress, s.where(from))
			}
			return
		}
		g = &game{id: s.newID(), addr: from, created: now}
		s.games[from] = g
		s.byID[g.id] = g
		s.perIP[from.Addr()]++
		log.Printf("game %08x '%s' created: %d/%d players, version %d, udp %v%s",
			g.id, info.Name, info.Players, info.MaxPlayers, info.Version, m.Flags&flagUDP != 0, s.where(from))
	} else if g.info != info {
		log.Printf("game %08x '%s': %d/%d players", g.id, info.Name, info.Players, info.MaxPlayers)
	}
	g.flags = m.Flags
	g.announcement = m.Announcement
	g.info = info
	g.lastSeen = now
	s.send(from, encodeRegistered(m.Nonce, g.id, s.cfg.RefreshMs, from))
}

func (s *Server) onUnregister(b []byte, from netip.AddrPort, now time.Time) {
	m, ok := decodeRequest(b)
	// No reply either way: the host sends it once while it shuts down, and the game expires anyway.
	if !ok || !s.validCookie(from, m.Cookie, now) {
		return
	}
	if g := s.games[from]; g != nil && g.id == m.Arg {
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
			entries = append(entries, gameEntry{ID: g.id, Addr: g.addr, Flags: g.flags, Announcement: g.announcement})
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
		s.send(from, encodeJoined(m.Nonce, m.Arg, netip.AddrPort{}, 0))
		return
	}
	// The host punches towards the player's address while the player starts its handshake towards the host's.
	s.send(g.addr, encodeIntroduce(g.id, from))
	s.send(from, encodeJoined(m.Nonce, g.id, g.addr, g.flags))
	log.Printf("join of game %08x '%s'%s", g.id, g.info.Name, s.where(from))
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

func (s *Server) remove(g *game) {
	delete(s.games, g.addr)
	delete(s.byID, g.id)
	if s.perIP[g.addr.Addr()]--; s.perIP[g.addr.Addr()] <= 0 {
		delete(s.perIP, g.addr.Addr())
	}
}

func (s *Server) expire(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, g := range s.games {
		if now.Sub(g.lastSeen) > s.cfg.GameTimeout {
			log.Printf("game %08x '%s' expired (no REGISTER for %v)", g.id, g.info.Name, s.cfg.GameTimeout)
			s.remove(g)
		}
	}
	for ip, b := range s.limits {
		if now.Sub(b.last) > time.Minute {
			delete(s.limits, ip)
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
			AgeSeconds: int64(now.Sub(g.created).Seconds()),
		})
	}
	return views
}
