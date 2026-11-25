#include "net/udp_protocol.h"

#include <chrono>
#include <initializer_list>
#include <mutex>
#include <random>

namespace
{
    std::mutex g_randomMutex;

    std::random_device& device()
    {
        static std::random_device rd;   // MSVC: the OS's cryptographic generator
        return rd;
    }

    uint64_t rotl(uint64_t x, int b)
    {
        return (x << b) | (x >> (64 - b));
    }

    // SipHash-2-4 of 64-bit words.
    uint64_t sipHash(uint64_t k0, uint64_t k1, std::initializer_list<uint64_t> words)
    {
        uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
        uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
        const auto round = [&] {
            v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
            v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
            v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
            v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
        };
        const auto compress = [&](uint64_t m) {
            v3 ^= m;
            round();
            round();
            v0 ^= m;
        };
        for (const uint64_t m : words)
        {
            compress(m);
        }
        compress(uint64_t{words.size() * 8} << 56);
        v2 ^= 0xff;
        for (int i = 0; i < 4; ++i)
        {
            round();
        }
        return v0 ^ v1 ^ v2 ^ v3;
    }

    struct Keys
    {
        uint64_t k0, k1, k2, k3;
    };

    const Keys& keys()
    {
        static const Keys k{UdpProto::random64(), UdpProto::random64(), UdpProto::random64(), UdpProto::random64()};
        return k;
    }

    void cookieFor(const Net::Address& from, uint32_t nonce, uint64_t period, uint8_t* out)
    {
        const Keys& k = keys();
        uint8_t wire[24] = {};
        Net::encode(from, wire);
        uint64_t m[3];
        std::memcpy(m, wire, sizeof(m));
        m[2] |= uint64_t{nonce} << 32;      // the port is in the low 16 bits
        const uint64_t a = sipHash(k.k0, k.k1, {m[0], m[1], m[2], period});
        const uint64_t b = sipHash(k.k2, k.k3, {m[0], m[1], m[2], period});
        std::memcpy(out, &a, 8);
        std::memcpy(out + 8, &b, 8);
    }

    constexpr uint64_t kCookiePeriodMs = 30000;
}

uint32_t UdpProto::random32()
{
    std::scoped_lock lock(g_randomMutex);
    return device()();
}

uint64_t UdpProto::random64()
{
    std::scoped_lock lock(g_randomMutex);
    return (uint64_t{device()()} << 32) | device()();
}

void UdpProto::cookie(const Net::Address& from, uint32_t nonce, uint8_t* out)
{
    cookieFor(from, nonce, nowMs() / kCookiePeriodMs, out);
}

bool UdpProto::validCookie(const Net::Address& from, uint32_t nonce, const uint8_t* c)
{
    const uint64_t period = nowMs() / kCookiePeriodMs;
    uint8_t expected[kCookieSize];
    for (const uint64_t p : {period, period - 1})
    {
        cookieFor(from, nonce, p, expected);
        if (std::memcmp(expected, c, kCookieSize) == 0)
        {
            return true;
        }
    }
    return false;
}

uint64_t UdpProto::nowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
