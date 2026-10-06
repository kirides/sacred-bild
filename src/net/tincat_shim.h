#pragma once

// [Net] Udp: TinCat's game connection on SacredBild's UDP transport. tincat2.dll's WSOCK32 imports are replaced so
// that a TCP socket TinCat connects (player) or accepts (gameserver) can be backed by a UdpTransport session:
// - player: connect() to a gameserver starts the UDP handshake instead; if the host doesn't answer or refuses, the
//   socket connects over TCP after all (select() does that for TinCat's non-blocking connect);
// - gameserver: the socket TinCat listens on also reports the players that connected over UDP, and accept() hands
//   them out with a placeholder socket handle.
// Every other call on those sockets (send, recv, select, shutdown, closesocket, options) goes to the session.
// TinCat's sockets are blocking once connected: one reader thread per connection blocks in recv(), one writer
// thread sends each message as two send() calls (28-byte header, payload).
namespace TincatShim
{
    // host: this is the gameserver. Call after Connection::install (whose hooks it chains to).
    void install(bool host);
}
