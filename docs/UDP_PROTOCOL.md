# SacredBild UDP transport and matchmaker protocol

Two protocols share SacredBild's UDP *endpoint* in each process: an IPv4 and an IPv6 socket (the IPv6 one where
the system has IPv6):

- **Transport**: Sacred's game connection (TinCat's TCP stream, see RE_NOTES "Networking: game connection") carried
  as a reliable, ordered byte stream over UDP (KCP), between a player's `sacred.exe` and the host's `gameserver.exe`.
  Both need SacredBild with `[Net] Udp=1`; otherwise the player falls back to TCP.
- **Matchmaker**: hosts publish their games at a matchmaking server (`matchmaker/`, Go), players list them in
  Sacred's LAN list and get introduced to the host (UDP hole punching).

The host's endpoint is bound to `[Net] Port` (2105, also the LAN relay's subscription port) in both families; a
player's sockets to ephemeral ports. The matchmaker listens on UDP 2107 by default, IPv4 and IPv6.

Sacred itself only knows IPv4 (TinCat's sockets, the LAN announcement's address field). Over the transport the game
never touches the network, so a session can run over IPv6: that reaches hosts without a public IPv4 address (CGNAT,
DS-Lite) and needs no address translation, so hole punching almost always works. Only the TCP fallback is IPv4.

## Conventions

- Every datagram starts with a 4-byte magic: 3 bytes protocol (`SBT` transport, `SBM` matchmaker, `SBL` LAN relay)
  and a type character.
- Integers are little-endian.
- `addr` (18 bytes): a 16-byte IPv6 address in network order, an IPv4 address as IPv4-mapped IPv6
  (`::ffff:a.b.c.d`), then the port as `u16`. All zero = no address.
- `version` is 1. A receiver drops datagrams with another version (transport: answers REFUSED).
- Datagrams are at most 1200 bytes (fits VPN tunnels without fragmentation).
- Every reply to an address that hasn't proven it receives at that address (cookie) is no larger than the request,
  so a spoofed source address can't be used for amplification.
- `cookie`: 16 bytes the server derives from the sender's address and a time bucket with a secret key; a request
  without a valid one is answered with CHALLENGE only. Clients keep it and send it with every request.

## Transport (`SBT`)

Client (player) C, host H (gameserver).

| Magic | Dir | Size | Layout |
|---|---|---|---|
| `SBTH` HELLO | C->H | 64 | +4 u32 version, +8 u32 clientNonce (random, != 0), +12 u16 tcpPort (the gameserver TCP port the game connects to), +14 u16 0, +16 cookie[16] (zero the first time), +32 zero padding |
| `SBTC` CHALLENGE | H->C | 32 | +4 version, +8 clientNonce, +12 u32 0, +16 cookie[16] |
| `SBTW` WELCOME | H->C | 32 | +4 version, +8 clientNonce, +12 u32 sessionId, +16 u64 token, +24 u64 0 |
| `SBTN` REFUSED | H->C | 16 | +4 version, +8 clientNonce, +12 u32 reason (1 transport off, 2 no game listening on tcpPort, 3 full, 4 version) |
| `SBTD` DATA | both | 16+n | +4 u32 sessionId, +8 u64 token, +16 KCP packet (conv = sessionId) |
| `SBTK` KEEPALIVE | both | 16 | +4 sessionId, +8 token |
| `SBTX` CLOSE | both | 16 | +4 sessionId, +8 token |
| `SBTP` PUNCH | both | 16 | +4 u32 gameId, +8 u64 0 — opens NAT mappings, ignored on arrival |

- Handshake: HELLO (no cookie) -> CHALLENGE -> HELLO (cookie) -> WELCOME. The client sends HELLO every 200 ms, each
  with the cookie that address gave it, to the host's addresses in the family `[Net] Prefer` names (IPv6 by default),
  and also to its addresses in the other family once the preferred one had 1 s without a WELCOME, refused, or has
  none. It stops at a WELCOME, a REFUSED from every address, or its time-out (1.5 s, 3 s for games from the
  matchmaker, 1 s more when the host has both families); then it connects over TCP instead. The first WELCOME
  decides the address the session starts on.
- The host creates a session only for a valid cookie, one per clientNonce: a HELLO with a known clientNonce (a
  repeat, or the same handshake over the other address family) gets that session's WELCOME again.
- sessionId and token are random (sessionId != 0). A DATA/KEEPALIVE/CLOSE is accepted only if both match.
- **Roaming**: the host answers to the address the last valid datagram came from, so a player whose address changes
  (Wi-Fi <-> mobile, NAT rebinding) keeps the session. A player that joined through the matchmaker and hears
  nothing from the host for 2 s sends JOIN again, so the host punches towards its new address.
- KEEPALIVE every second without other traffic, every 200 ms while nothing arrives from the other side (so both
  notice within a round trip when a link comes back). A session that receives nothing for 45 s is dead; the game sees a
  connection reset.
- KCP: stream mode; each TinCat message (header and payload) is handed to KCP and flushed once complete. A receiver
  reads the KCP messages as one byte stream, so senders in message mode work as well. nodelay 1, interval 10 ms, fast resend once a later segment is acknowledged, no congestion window, send window
  128, receive window 256, MTU 1200 - 16. Segments are resent at once when the peer is heard again after 300 ms of
  silence (KCP's own back-off would wait seconds after an outage).

## Matchmaker (`SBM`)

Host H (gameserver), player C (sacred.exe), matchmaker M. M only ever answers the address a request came from,
and sends INTRODUCE to registered hosts only.

| Magic | Dir | Size | Layout |
|---|---|---|---|
| `SBMC` CHALLENGE | M->H/C | 32 | +4 version, +8 u32 nonce (from the request), +12 u32 0, +16 cookie[16] |
| `SBMR` REGISTER | H->M | 214 | +4 version, +8 u32 nonce, +12 u16 flags (bit 0: accepts the UDP transport), +14 u16 0, +16 cookie[16], +32 u64 hostKey, +40 announcement[0xAE] |
| `SBMA` REGISTERED | M->H | 40 | +4 version, +8 nonce, +12 u32 gameId, +16 u32 refreshMs, +20 addr (the host's address as M sees it, in the family the REGISTER came over), +38 u16 0 |
| `SBMU` UNREGISTER | H->M | 32 | +4 version, +8 nonce, +12 u32 gameId, +16 cookie[16] |
| `SBML` LIST | C->M | 32 | +4 version, +8 nonce, +12 u16 page, +14 u16 0, +16 cookie[16] |
| `SBMG` GAMES | M->C | 20+218n | +4 version, +8 nonce, +12 u16 page, +14 u16 pageCount, +16 u16 count (<= 5), +18 u16 0, then count entries: u32 gameId, u16 flags, u16 0, addr4 (the host's IPv4 address or zero), addr6 (its IPv6 address or zero), announcement[0xAE] |
| `SBMJ` JOIN | C->M | 32 | +4 version, +8 nonce, +12 u32 gameId, +16 cookie[16] |
| `SBMO` JOINED | M->C | 56 | +4 version, +8 nonce, +12 u32 gameId, +16 u16 flags, +18 u16 0, +20 addr4, +38 addr6; both zero = no such game |
| `SBMI` INTRODUCE | M->H | 32 | +4 version, +8 u32 gameId, +12 addr (the player's address as M sees it), +30 u16 0 |

- `announcement` is Sacred's plain LAN announcement (0xAE bytes, see RE_NOTES "Networking: LAN games"):
  +0 u16 version check, +2 u16 gameserver TCP port, +4 u32 IPv4 (ignored, M doesn't use it), +8 u32 flags,
  +0xC u8 players, +0xD u8 max players, +0xE wchar_t name[80] (UTF-16LE, zero-terminated if shorter).
- Hosts and players send every request over each family they have an address of M for (IPv4 and IPv6); M sees a
  different address for each, so cookies are kept per family.
- REGISTER every `refreshMs` (5000) over both families while the game runs; it also keeps the host's NAT mapping and
  firewall open. A game is identified by `hostKey` (random per gameserver, known only to the host and M): the
  first REGISTER creates it, later ones update it, and each family's address is the source of the latest REGISTER
  over that family. M forgets a family's address 20 s after its last REGISTER, and the game when it has none left,
  or on UNREGISTER (accepted from either address).
- Limits (requests per second, games per address) count per IPv4 address and per IPv6 /64 network.
- LIST pages hold 5 games each; the player asks for page 0 every 2 s while Sacred's LAN list is open, and for the
  other pages when pageCount says there are more. Each game becomes a LAN announcement: with the IPv4 address of
  addr4, or for a game without one a placeholder from 198.18.0.0/15 that SacredBild maps back to the game (such a
  game can only be joined over the UDP transport; without flag bit 0 it isn't listed).
- JOIN: M answers JOINED, and sends INTRODUCE with the player's address to the host's address in the same family,
  if the game has one. Both then send PUNCH to each other (the host 3 times over 300 ms), and the player starts the
  transport handshake to all of the host's addresses.

## Matchmaker server

- Listens on IPv4 and IPv6 (`Listen=:2107`).
- Web UI (HTTP) with the current games: name, players / max, game version, UDP, IPv4 / IPv6 reachability, age. It
  never shows addresses.
  `/api/games` returns the same as JSON.
- Logs no addresses unless `LogSensitiveData=1`.
