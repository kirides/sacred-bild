#pragma once
#include <cstdint>

// Addresses for the gameserver.exe that ships with the German Sacred Gold build (PE timestamp 0x451BBDBF).
// Names match the Ghidra program "/gameserver.exe". sacred.exe starts it for the games it hosts.
namespace GameServer::Addr
{
    constexpr uint32_t kTimestamp = 0x451BBDBF;
    constexpr uint32_t kSizeOfImage = 0x0068B000;
    constexpr uint32_t kEntryPoint = 0x00112F94;

    // Application object; +0x40 the network object (cNetServer_ctor 0x4E8170).
    constexpr uintptr_t g_pApp = 0x00634238;
    constexpr uintptr_t app_net = 0x40;

    // Network object. cNetServer_initNetwork (0x4E8AB0) creates the ping socket: UDP, SO_BROADCAST, connected to
    // 255.255.255.255:<ping port>. cNetServer_sendAnnouncement (0x4E99A0) fills the plain announcement, compresses
    // it and sends it with the only send() call in the exe; the version field is cleared again right after.
    constexpr uintptr_t net_pingSocket = 0x1A8;
    constexpr uintptr_t net_announcement = 0x596C;
}
