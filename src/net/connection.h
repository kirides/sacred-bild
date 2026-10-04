#pragma once

// The game connection (TinCat over TCP). The data flow option (MODEM/ISDN or LAN) decides in cGCclass_initNetwork
// whether the client's connection runs with TCP_NODELAY: only for LAN. With MODEM/ISDN, Nagle's algorithm holds
// back each small message while the previous one is unacknowledged, adding up to a round trip (more with delayed
// ACKs) to everything the client sends. The server always uses TCP_NODELAY.
namespace Connection
{
    // [Net] NoDelay: TCP_NODELAY in both modes.
    void install();
}
