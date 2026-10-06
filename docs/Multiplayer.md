# Multiplayer

LAN games over VPNs, a faster game connection over UDP, and a matchmaker that turns the LAN list into a global
lobby. The `[Net]` settings are in [`SacredBild.ini`](Configuration.md).

## What SacredBild changes

- **LAN games over VPNs**: Sacred finds LAN games through broadcasts that Windows sends on one network adapter only
  and that many VPNs don't carry, and the announced address is one Sacred picked from the first three adapters it
  found. When Sacred starts the gameserver for a hosted game, SacredBild goes along: every announcement goes out on
  every adapter with that adapter's own address, and to players who list the host in `[Net] Hosts`.
- **Game connection over UDP** (`[Net] Udp`, optional): Sacred's game connection is TCP, which waits at least 300 ms
  (doubling on every further loss) before it resends a lost packet, and after a loss holds everything behind it
  back; distant and wireless connections show that as stalls and rubber banding. With SacredBild on both sides, the
  connection runs over UDP instead (the same byte stream, kept in order by [KCP](https://github.com/skywind3000/kcp)):
  a lost packet is resent after about one round trip, the connection picks up within a round trip after an outage,
  and it survives the player's address changing (Wi-Fi to mobile, a new address from the provider). Joining a host
  without it connects over TCP as before. Every game connection logs its round trip and resends when it closes.
- **Matchmaker** (`[Net] Matchmaker`, `matchmaker/`): a small server (Go; Linux, Windows, macOS) that lists the
  games of everyone using it in Sacred's own LAN list, so that LAN mode becomes a global lobby. Joining a game gets
  the player introduced to the host, and both open their way to each other (UDP hole punching), so hosts don't need
  port forwarding with the UDP connection. Over IPv6 as well as IPv4, so hosts without a public IPv4 address (CGNAT,
  DS-Lite) can host too. Its web page shows the games (name, players), never addresses, and it logs no addresses
  unless told to.

## LAN games over a VPN

Everyone installs SacredBild. The host creates the LAN game as usual; Windows Firewall has to let `gameserver.exe`
receive on the VPN adapter (the prompt on first start, or a rule for its TCP port and UDP 2105; VPN adapters are
often in the "Public" profile).

- VPNs that carry broadcasts (ZeroTier, Hamachi, Radmin VPN, OpenVPN TAP): the game shows up in the LAN list.
- VPNs without broadcasts (WireGuard, Tailscale, OpenVPN TUN): joining players add the host's VPN address, e.g.
  `Hosts=10.8.0.2` or a Tailscale name. A shared list of all players works, a PC skips its own addresses.

The host's relay logs to `SacredBild-server.log`, the LAN list to `SacredBild.log` (lines starting with `LAN`).

## Online games (matchmaker)

Everyone sets `[Net] Matchmaker` to the same server and `Udp=1` (the settings window: Network tab). The host creates
a LAN game as usual; it shows up in everyone's LAN list, with the host's public address. Joining it:

1. the player asks the matchmaker to introduce it to the host; both send each other a few UDP packets, which opens
   their routers (most home routers; two players behind the strictest kind, e.g. some mobile networks on both
   sides, still need port forwarding);
2. the game connection runs over UDP to the host's `[Net] Port`: over the family `[Net] Prefer` names (IPv6 by
   default) first, and over the other one as well if that has no answer within a second (whichever answers first
   then); if neither answers within 3 s (4 s with both), the player connects over TCP to the game's port, which
   then needs to be forwarded on the host.

IPv6 needs no address translation, so step 1 almost always works there, and it reaches hosts that have no public
IPv4 address of their own (mobile networks' CGNAT, DS-Lite cable and fibre connections). Both sides need IPv6 for
that, and the matchmaker an IPv6 address (an AAAA record). Sacred itself only knows IPv4: a game the matchmaker
reaches over IPv6 only is listed with a stand-in address from 198.18.0.0/15 (Sacred has 4 bytes for a game's
address, and its LAN list tells games apart by address and name; the address is not shown in game). Such a game can
only be joined over UDP: if that fails, joining fails right away. `SacredBild.log` names the game behind each
stand-in.

In the LAN list, matchmaker games on the UDP connection whose host IPv6 reaches show that in front of their name:
`[IPv4+6]`, or `[IPv6]` (only over IPv6). All other games keep their name.

Players behind the same router as the host see the game twice; the LAN one is the one to join. The host's
gameserver logs the matchmaker and its players to `SacredBild-server.log`, a player's joins go to `SacredBild.log`
(lines starting with `Matchmaker` and `UDP`).

Running a matchmaker: see [`matchmaker/README.md`](../matchmaker/README.md) (one executable, UDP 2107 and a web page
on 8080). The protocol is described in [`UDP_PROTOCOL.md`](UDP_PROTOCOL.md).
