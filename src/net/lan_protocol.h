#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// LAN game announcements. A gameserver sends one every few seconds (and when players join or leave) as a UDP
// datagram to 255.255.255.255:<ping port>; the LAN list in sacred.exe listens on that port and connects to the
// address inside the announcement, not to the sender's.
namespace LanAnnounce
{
    // Plain announcement (gameserver: network object + 0x596C):
    //   +0x00 u16 version check, +0x02 u16 gameserver TCP port, +0x04 u32 IPv4 address (host byte order),
    //   +0x08 u32 flags, +0x0C u8 players, +0x0D u8 max players, +0x0E wchar_t name[80]
    constexpr size_t kSize = 0xAE;
    constexpr size_t kAddressOffset = 0x04;
    constexpr size_t kNameOffset = 0x0E;
    constexpr size_t kNameChars = (kSize - kNameOffset) / 2;

    // On the wire it goes through the game's CompressMemory: a u32 header (payload size, bit 27 = not compressed)
    // and the payload, zlib-compressed or XOR-chained. Announcements are compressed; UncompressMemory accepts both.
    constexpr size_t kWireSize = 4 + kSize;

    // Writes `plain` with its address replaced by `address` (network byte order) in the uncompressed wire format.
    void encode(const uint8_t* plain, uint32_t address, uint8_t* wire);

    std::string gameName(const uint8_t* plain);    // UTF-8
}

// SacredBild's own messages, for hosts whose broadcasts can't reach a player (VPNs without broadcasts): the player
// subscribes at the host's relay port, and the host sends its announcements to its subscribers as well.
namespace LanRelay
{
    constexpr uint8_t kSubscribeMagic[4] = {'S', 'B', 'L', 'S'};
    constexpr uint8_t kAnnounceMagic[4] = {'S', 'B', 'L', 'A'};
    constexpr uint32_t kVersion = 1;

    // magic, version, zero padding: at least as large as the answer, so the relay can't amplify spoofed requests.
    constexpr size_t kSubscribeSize = 192;
    // magic, version, plain announcement (the subscriber fills in the address it received it from).
    constexpr size_t kAnnounceSize = 8 + LanAnnounce::kSize;

    constexpr uint32_t kSubscriptionMs = 10000;  // the host sends announcements this long after a subscription
    constexpr uint32_t kResubscribeMs = 2000;    // the player renews it this often while the LAN list is open

    bool hasHeader(const uint8_t* msg, size_t size, const uint8_t (&magic)[4], size_t minSize);
    void writeHeader(uint8_t* msg, const uint8_t (&magic)[4]);
}
