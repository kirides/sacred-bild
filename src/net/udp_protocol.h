#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire formats of SacredBild's UDP transport (SBT) and of the matchmaker (SBM); docs/UDP_PROTOCOL.md describes
// them, matchmaker/ implements the server side. Little-endian integers, IPv4 addresses as 4 bytes in network order.
namespace UdpProto
{
    constexpr uint32_t kVersion = 1;
    constexpr size_t kMaxDatagram = 1200;
    constexpr size_t kCookieSize = 16;

    // Magic: 3 bytes protocol, 1 byte type.
    constexpr char kTransport[3] = {'S', 'B', 'T'};
    constexpr char kMatchmaker[3] = {'S', 'B', 'M'};
    constexpr char kLanRelay[3] = {'S', 'B', 'L'};

    namespace T     // transport types
    {
        constexpr char Hello = 'H', Challenge = 'C', Welcome = 'W', Refused = 'N', Data = 'D', KeepAlive = 'K',
            Close = 'X', Punch = 'P';
        constexpr size_t kHelloSize = 64, kChallengeSize = 32, kWelcomeSize = 32, kRefusedSize = 16;
        constexpr size_t kHeaderSize = 16;      // DATA / KEEPALIVE / CLOSE / PUNCH: magic, u32 session, u64 token

        enum Refusal : uint32_t
        {
            Disabled = 1,
            NotListening = 2,
            Full = 3,
            BadVersion = 4,
        };
    }

    namespace M     // matchmaker types
    {
        constexpr char Challenge = 'C', Register = 'R', Registered = 'A', Unregister = 'U', List = 'L', Games = 'G',
            Join = 'J', Joined = 'O', Introduce = 'I';
        constexpr size_t kChallengeSize = 32, kRegisterSize = 32 + 0xAE, kRegisteredSize = 32, kUnregisterSize = 32,
            kListSize = 32, kGamesHeader = 20, kGameEntry = 12 + 0xAE, kJoinSize = 32, kJoinedSize = 24,
            kIntroduceSize = 24;
        constexpr uint16_t kFlagUdp = 1;        // the host accepts the UDP transport
    }

    inline bool is(const uint8_t* p, size_t n, const char (&proto)[3], char type)
    {
        return n >= 4 && std::memcmp(p, proto, 3) == 0 && p[3] == static_cast<uint8_t>(type);
    }

    inline void magic(uint8_t* p, const char (&proto)[3], char type)
    {
        std::memcpy(p, proto, 3);
        p[3] = static_cast<uint8_t>(type);
    }

    template <class T>
    T get(const uint8_t* p, size_t offset)
    {
        T v;
        std::memcpy(&v, p + offset, sizeof(T));
        return v;
    }

    template <class T>
    void put(uint8_t* p, size_t offset, T v)
    {
        std::memcpy(p + offset, &v, sizeof(T));
    }

    // Random values from the OS's generator (session ids, tokens, nonces, keys).
    uint32_t random32();
    uint64_t random64();

    // 16-byte cookie for (address, port, nonce) in the current or previous 30 s period, keyed per process.
    void cookie(uint32_t address, uint16_t port, uint32_t nonce, uint8_t* out);
    bool validCookie(uint32_t address, uint16_t port, uint32_t nonce, const uint8_t* cookie);

    // Monotonic milliseconds (the transport's clock; KCP takes its low 32 bits).
    uint64_t nowMs();
}
