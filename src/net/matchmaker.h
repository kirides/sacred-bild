#pragma once
#include <cstdint>
#include <vector>

#include <winsock2.h>

// Client of the matchmaking server ([Net] Matchmaker, matchmaker/ in this repository, docs/UDP_PROTOCOL.md).
// Hosting: the gameserver publishes the game Sacred announces on the LAN ([Net] Publish), which also keeps its NAT
// mapping open, and punches towards players the matchmaker introduces. Joining: the games listed there show up in
// Sacred's LAN list, and joining one asks the matchmaker to introduce this player to the host (UDP hole punching).
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
        sockaddr_in host;       // the host's address as the matchmaker sees it (UDP endpoint)
    };
    void poll(std::vector<Listed>& out);

    // Player: the matchmaker game at the address the game connects to (from the LAN list).
    struct Target
    {
        uint32_t gameId;
        sockaddr_in udp;        // the host's endpoint
        bool udpOk;             // the host accepts the UDP transport
    };
    bool lookup(uint32_t address, uint16_t tcpPort, Target& out);

    // Player: asks the matchmaker to introduce this player to the host of `gameId` (rate-limited).
    void join(uint32_t gameId);
}
