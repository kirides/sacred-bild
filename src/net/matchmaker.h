#pragma once
#include "net/address.h"

#include <cstdint>
#include <vector>

// Client of the matchmaking server ([Net] Matchmaker, matchmaker/ in this repository, docs/UDP_PROTOCOL.md), over
// IPv4 and IPv6 where both work. Hosting: the gameserver publishes the game Sacred announces on the LAN
// ([Net] Publish), which also keeps its NAT mapping and firewall open, and punches towards players the matchmaker
// introduces. Joining: the games listed there show up in Sacred's LAN list, and joining one asks the matchmaker to
// introduce this player to the host (UDP hole punching).
namespace Matchmaker
{
    bool enabled();

    // Registers the endpoint handlers; host: this is the gameserver.
    void install(bool host);

    // Host: the plain announcement (LanAnnounce::kSize bytes), every time the game announces itself.
    void publish(const uint8_t* plain);

    // Player, from the LAN list's poll: asks for the list now and then; returns the games to announce now.
    struct Listed
    {
        uint8_t plain[0xAE];
        // The address to announce (network order): the host's IPv4 address, or for a host reachable over IPv6 only
        // a stand-in from 198.18.0.0/15 that lookup() maps back to the game.
        uint32_t address;
    };
    void poll(std::vector<Listed>& out);

    // Player: the matchmaker game at the address the game connects to (from the LAN list).
    struct Target
    {
        uint32_t gameId;
        Net::Address ipv4;      // the host's endpoints as the matchmaker sees them; unset = none
        Net::Address ipv6;
        bool udpOk;             // the host accepts the UDP transport
    };
    bool lookup(uint32_t address, uint16_t tcpPort, Target& out);

    // Player: asks the matchmaker to introduce this player to the host of `gameId` (rate-limited).
    void join(uint32_t gameId);

    // A stand-in address (network order): such a game is reachable over the UDP transport only.
    bool isStandIn(uint32_t address);
}
