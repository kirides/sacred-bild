#pragma once
#include <string>

// [Net] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Net
    {
        // LAN games over VPNs. Hosting: the gameserver Sacred starts gets SacredBild as well; it announces the game on
        // every network adapter with that adapter's address and to SacredBild players that subscribe at UDP `port`.
        bool relay = true;
        int port = 2105;
        // Joining: hosts whose games are listed even though their broadcasts don't arrive here
        // (comma-separated IPv4 addresses or host names, optionally with :port).
        std::string hosts;
        // TCP_NODELAY on the game connection in both data flow modes; the game uses it for LAN only, MODEM/ISDN runs
        // with Nagle's algorithm (small messages wait for the previous one's ACK).
        bool noDelay = true;
        // Seconds a player connecting to a gameserver Sacred started has to send its first message (the game: 5).
        int joinTimeout = 30;
        // The game connection over SacredBild's UDP transport (KCP) when the other side has it on too; otherwise TCP.
        // Hosting: the gameserver accepts it on UDP `port`.
        bool udp = false;
        // Matchmaking server (host[:port], UDP; empty = none): its games are listed in the LAN list, and joining one
        // gets the player introduced to the host (hole punching for the UDP transport).
        std::string matchmaker;
        // Hosting: games are published at the matchmaker.
        bool publish = true;
        // Joining over UDP a host with IPv4 and IPv6 addresses ([Net] Prefer=IPv6 / IPv4): the handshake tries this
        // family first and the other one too if it has no answer within a second.
        bool preferIpv6 = true;
    };
    inline Net net;
}
