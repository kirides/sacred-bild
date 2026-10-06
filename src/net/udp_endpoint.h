#pragma once
#include <cstdint>

struct sockaddr_in;

// SacredBild's UDP socket in this process, shared by the transport (SBT), the matchmaker client (SBM) and, in the
// gameserver, the LAN relay's subscriptions (SBL). The host's is bound to [Net] Port, a player's to any free port.
// One I/O thread receives and hands each datagram to the handler of its protocol, and runs the ticks.
namespace UdpEndpoint
{
    using Handler = void (*)(const uint8_t* data, int size, const sockaddr_in& from);
    // Called on the I/O thread after every wake-up; returns the milliseconds until it wants to run again.
    using Tick = uint32_t (*)(uint64_t nowMs);

    // Register before open().
    void onPacket(const char (&protocol)[3], Handler handler);
    void onTick(Tick tick);

    // Opens the socket and starts the I/O thread once; later calls return whether that worked. Not from DllMain
    // (Winsock loads on first use).
    bool open(uint16_t port, bool broadcast);
    bool isOpen();
    uint16_t port();

    bool sendTo(const void* data, int size, const sockaddr_in& to);
    // Runs the ticks now (e.g. after queueing work for them).
    void wake();
}
