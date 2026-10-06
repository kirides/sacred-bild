# SacredBild UDP transport and matchmaker protocol

Two protocols share one UDP socket per process (SacredBild's *endpoint*):

- **Transport**: Sacred's game connection (TinCat's TCP stream, see RE_NOTES "Networking: game connection") carried
  as a reliable, ordered byte stream over UDP (KCP), between a player's `sacred.exe` and the host's `gameserver.exe`.
  Both need SacredBild with `[Net] Udp=1`; otherwise the player falls back to TCP.
- **Matchmaker**: hosts publish their games at a matchmaking server (`matchmaker/`, Go), players list them in
  Sacred's LAN list and get introduced to the host (UDP hole punching).

The host's endpoint is bound to `[Net] Port` (2105, also the LAN relay's subscription port); a player's endpoint to
an ephemeral port. The matchmaker listens on UDP 2107 by default.

## Conventions

- Every datagram starts with a 4-byte magic: 3 bytes protocol (`SBT` transport, `SBM` matchmaker, `SBL` LAN relay)
  and a type character.
- Integers are little-endian. IPv4 addresses are 4 raw bytes in network order (`a.b.c.d` -> `a, b, c, d`).
  Ports are `u16` little-endian.
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

- Handshake: HELLO (no cookie) -> CHALLENGE -> HELLO (cookie) -> WELCOME. The host creates the session only for a
  valid cookie; a repeated HELLO with the same (address, clientNonce) gets the same WELCOME again. The client sends
  HELLO every 200 ms until WELCOME, REFUSED or its time-out (1.5 s, 3 s for games from the matchmaker), then
  connects over TCP instead.
- sessionId and token are random (sessionId != 0). A DATA/KEEPALIVE/CLOSE is accepted only if both match.
- **Roaming**: the host answers to the address the last valid datagram came from, so a player whose address changes
  (Wi-Fi <-> mobile, NAT rebinding) keeps the session. A player that joined through the matchmaker and hears
  nothing from the host for 2 s sends JOIN again, so the host punches towards its new address.
- KEEPALIVE every second without other traffic, every 200 ms while nothing arrives from the other side (so both
  notice within a round trip when a link comes back). A session that receives nothing for 45 s is dead; the game sees a
  connection reset.
- KCP: stream mode, nodelay 1, interval 10 ms, fast resend once a later segment is acknowledged, no congestion window, send window
  128, receive window 256, MTU 1200 - 16. Segments are resent at once when the peer is heard again after 300 ms of
  silence (KCP's own back-off would wait seconds after an outage).

## Matchmaker (`SBM`)

Host H (gameserver), player C (sacred.exe), matchmaker M. M only ever answers the address a request came from,
and sends INTRODUCE to registered hosts only.

| Magic | Dir | Size | Layout |
|---|---|---|---|
| `SBMC` CHALLENGE | M->H/C | 32 | +4 version, +8 u32 nonce (from the request), +12 u32 0, +16 cookie[16] |
| `SBMR` REGISTER | H->M | 206 | +4 version, +8 u32 nonce, +12 u16 flags (bit 0: accepts the UDP transport), +14 u16 0, +16 cookie[16], +32 announcement[0xAE] |
| `SBMA` REGISTERED | M->H | 32 | +4 version, +8 nonce, +12 u32 gameId, +16 u32 refreshMs, +20 ip[4], +24 u16 port (the host's address as M sees it), +26 6 bytes 0 |
| `SBMU` UNREGISTER | H->M | 32 | +4 version, +8 nonce, +12 u32 gameId, +16 cookie[16] |
| `SBML` LIST | C->M | 32 | +4 version, +8 nonce, +12 u16 page, +14 u16 0, +16 cookie[16] |
| `SBMG` GAMES | M->C | 20+186n | +4 version, +8 nonce, +12 u16 page, +14 u16 pageCount, +16 u16 count (<= 6), +18 u16 0, then count entries: u32 gameId, ip[4], u16 port, u16 flags, announcement[0xAE] |
| `SBMJ` JOIN | C->M | 32 | +4 version, +8 nonce, +12 u32 gameId, +16 cookie[16] |
| `SBMO` JOINED | M->C | 24 | +4 version, +8 nonce, +12 u32 gameId, +16 ip[4], +20 u16 port, +22 u16 flags; ip 0 = no such game |
| `SBMI` INTRODUCE | M->H | 24 | +4 version, +8 u32 gameId, +12 ip[4], +16 u16 port (the player's address as M sees it), +18 6 bytes 0 |

- `announcement` is Sacred's plain LAN announcement (0xAE bytes, see RE_NOTES "Networking: LAN games"):
  +0 u16 version check, +2 u16 gameserver TCP port, +4 u32 IPv4 (ignored, M doesn't use it), +8 u32 flags,
  +0xC u8 players, +0xD u8 max players, +0xE wchar_t name[80] (UTF-16LE, zero-terminated if shorter).
- REGISTER every `refreshMs` (5000) while the game runs; it also keeps the host's NAT mapping open. The first one
  (and one after M restarted) creates the game, later ones update it. A game is identified by the host's address
  (ip, port); M drops it 20 s after its last REGISTER, or on UNREGISTER.
- LIST pages hold 6 games each; the player asks for page 0 every 2 s while Sacred's LAN list is open, and for the
  other pages when pageCount says there are more. Each game becomes a LAN announcement with `ip` as its address.
- JOIN: M sends INTRODUCE to the host and JOINED to the player. Both then send PUNCH to each other (the host 3 times
  over 300 ms), and the player starts the transport handshake to (ip, port).

## Matchmaker server

- Web UI (HTTP) with the current games: name, players / max, game version, UDP, age. It never shows addresses.
  `/api/games` returns the same as JSON.
- Logs no addresses unless `LogSensitiveData=1`.
