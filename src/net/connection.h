#pragma once

// The game connection (TinCat over TCP). The data flow option (MODEM/ISDN or LAN) decides in cGCclass_initNetwork
// whether the client's connection runs with TCP_NODELAY: only for LAN. With MODEM/ISDN, Nagle's algorithm holds
// back each small message while the previous one is unacknowledged, adding up to a round trip (more with delayed
// ACKs) to everything the client sends. The server always uses TCP_NODELAY.
// TinCat sets the option while its non-blocking connect is still under way, which Windows refuses (WSAEINVAL) and
// TinCat turns into a failed join (code -20) whenever the handshake takes longer than a LAN's; SacredBild then sets it
// before the connection's first send.
namespace Connection
{
    // tincat2.dll's setsockopt/send/closesocket imports for the above; [Net] NoDelay: TCP_NODELAY in both modes.
    void install();
}
