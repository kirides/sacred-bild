#pragma once
#include "net/address.h"

#include <cstdint>

// SacredBild's UDP endpoint in this process, shared by the transport (SBT), the matchmaker client (SBM) and, in the
// gameserver, the LAN relay's subscriptions (SBL): an IPv4 socket and, where the system has IPv6, an IPv6 socket.
// The host's are bound to [Net] Port, a player's to free ports. One I/O thread receives on both and hands each
// datagram to the handler of its protocol, and runs the ticks.
namespace UdpEndpoint
{
    using Handler = void (*)(const uint8_t* data, int size, const Net::Address& from);
    // Called on the I/O thread after every wake-up; returns the milliseconds until it wants to run again.
    using Tick = uint32_t (*)(uint64_t nowMs);

    // Register before open().
    void onPacket(const char (&protocol)[3], Handler handler);
    void onTick(Tick tick);

    // Opens the sockets and starts the I/O thread once; later calls return whether that worked (the IPv4 socket is
    // required, the IPv6 one optional). Not from DllMain (Winsock loads on first use).
    bool open(uint16_t port, bool broadcast);
    bool isOpen();
    bool hasIpv6();
    uint16_t port();        // the IPv4 socket's

    // Through the socket of `to`'s family; false if there is none.
    bool sendTo(const void* data, int size, const Net::Address& to);
    // Runs the ticks now (e.g. after queueing work for them).
    void wake();

    // tools/nettest: drops the transport's datagrams (SBT) that arrive over this family (AF_INET, AF_INET6; 0 = none),
    // like a path that is broken for the game connection.
    void dropReceived(int family);
}
