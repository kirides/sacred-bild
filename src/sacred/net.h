#pragma once
#include "game/gameserver_addr.h"
#include "game/sacred_addr.h"

#include <cstddef>
#include <cstdint>

// The network objects: the game client in sacred.exe, the application and its network object in gameserver.exe.
namespace Sacred
{
    // The game client (one instance). initNetwork (ENG 007D2BD0) binds a UDP socket (SO_REUSEADDR) to
    // NETWORK_PORT_LISTEN for the LAN list; pollLanGames (007D35F0) reads the gameservers' announcements from it with
    // select/__WSAFDIsSet/recvfrom (their only use in the exe) and drops games that were silent for 5 s. joinLanGame
    // (007D3D00) connects to the address inside the announcement.
    struct cGCclass
    {
        static constexpr uint32_t lan = 2;      // dataFlow

        uint8_t _00[0x14];
        uintptr_t lanSocket;        // SOCKET
        uint8_t _18[0x40414 - 0x18];
        uint32_t dataFlow;          // 2 = LAN; else MODEM/ISDN (as the options' NETWORK_SPEEDSETTINGS)

        // nullptr before the game made it.
        static cGCclass* instance() { return *Addr::g_pGameClient; }
    };
    static_assert(offsetof(cGCclass, lanSocket) == 0x14 && offsetof(cGCclass, dataFlow) == 0x40414);

    // The players of a network game (no RTTI; constructor ENG 007D7A70, made on first use by 007D84A0): up to 16
    // slots.
    struct cNetPlayers
    {
        // How many slots hold a player; 0 without a connected game client.
        uint32_t count() { return Addr::cNetPlayers_count(this) & 0xFFFF; }

        // nullptr before the game made it.
        static cNetPlayers* instance() { return *Addr::g_pNetPlayers; }
    };
}

namespace GameServer
{
    // The network object (constructor ENG 004E8170). initNetwork (004E8AB0) creates the ping socket: UDP,
    // SO_BROADCAST, connected to 255.255.255.255:<ping port>. sendAnnouncement (004E99A0) fills the plain announcement,
    // compresses it and sends it with the only send() call in the exe; the version field is cleared again right after.
    struct cNetServer
    {
        uint8_t _00[0x1A8];
        uintptr_t pingSocket;       // SOCKET
        uint8_t _1ac[0x596C - 0x1AC];
        uint8_t announcement[0xAE]; // the plain announcement (net/lan_protocol.h, LanAnnounce)
    };
    static_assert(offsetof(cNetServer, pingSocket) == 0x1A8 && offsetof(cNetServer, announcement) == 0x596C);

    // The application object.
    struct cApp
    {
        uint8_t _00[0x40];
        cNetServer* net;

        static cApp* instance() { return *Addr::g_pApp; }
    };
    static_assert(offsetof(cApp, net) == 0x40);
}
