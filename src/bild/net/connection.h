#pragma once

// The game connection (TinCat over TCP). The data flow option (MODEM/ISDN or LAN) decides in cGCclass_initNetwork
// whether the client's connection runs with TCP_NODELAY: only for LAN. With MODEM/ISDN, Nagle's algorithm holds
// back each small message while the previous one is unacknowledged, adding up to a round trip (more with delayed
// ACKs) to everything the client sends. The server always uses TCP_NODELAY.
// TinCat sets the option while its non-blocking connect is still under way, which Windows refuses (WSAEINVAL) and
// TinCat turns into a failed join (code -20) whenever the handshake takes longer than a LAN's; SacredBild then sets it
// before the connection's first send.
// In both processes, each TinCat message (28-byte header, payload: two send() calls) goes out with one send(), and
// a closing connection logs what TCP went through (RTT, resends, time-outs).
namespace Connection
{
    // tincat2.dll's send/closesocket imports, and setsockopt for the above (sacred.exe); [Net] NoDelay: TCP_NODELAY
    // in both modes. host: this is the gameserver.
    void install(bool host);
}
