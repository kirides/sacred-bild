#include "net/address.h"
#include "fmt.h"

#include <cstring>

namespace
{
    constexpr uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
}

Net::Address Net::ipv4(uint32_t address, uint16_t port)
{
    Address a{};
    a.Ipv4.sin_family = AF_INET;
    a.Ipv4.sin_addr.s_addr = address;
    a.Ipv4.sin_port = htons(port);
    return a;
}

Net::Address Net::fromSockaddr(const sockaddr* s)
{
    Address a{};
    if (s && s->sa_family == AF_INET)
    {
        a.Ipv4 = *reinterpret_cast<const sockaddr_in*>(s);
    }
    else if (s && s->sa_family == AF_INET6)
    {
        a.Ipv6 = *reinterpret_cast<const sockaddr_in6*>(s);
    }
    return a;
}

bool Net::isSet(const Address& a)
{
    return a.si_family == AF_INET || a.si_family == AF_INET6;
}

bool Net::isV6(const Address& a)
{
    return a.si_family == AF_INET6;
}

bool Net::same(const Address& a, const Address& b)
{
    if (a.si_family != b.si_family)
    {
        return false;
    }
    if (a.si_family == AF_INET)
    {
        return a.Ipv4.sin_addr.s_addr == b.Ipv4.sin_addr.s_addr && a.Ipv4.sin_port == b.Ipv4.sin_port;
    }
    if (a.si_family == AF_INET6)
    {
        return std::memcmp(&a.Ipv6.sin6_addr, &b.Ipv6.sin6_addr, sizeof(IN6_ADDR)) == 0 &&
            a.Ipv6.sin6_port == b.Ipv6.sin6_port;
    }
    return true;
}

uint16_t Net::port(const Address& a)
{
    return ntohs(a.si_family == AF_INET6 ? a.Ipv6.sin6_port : a.Ipv4.sin_port);
}

int Net::length(const Address& a)
{
    return a.si_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

std::string Net::toString(const Address& a)
{
    char text[INET6_ADDRSTRLEN] = {};
    if (a.si_family == AF_INET)
    {
        inet_ntop(AF_INET, &a.Ipv4.sin_addr, text, sizeof(text));
        return Fmt::format("{}:{}", text, port(a));
    }
    if (a.si_family == AF_INET6)
    {
        inet_ntop(AF_INET6, &a.Ipv6.sin6_addr, text, sizeof(text));
        return Fmt::format("[{}]:{}", text, port(a));
    }
    return "(none)";
}

void Net::encode(const Address& a, uint8_t* out)
{
    std::memset(out, 0, kWireSize);
    if (a.si_family == AF_INET)
    {
        std::memcpy(out, kMappedPrefix, sizeof(kMappedPrefix));
        std::memcpy(out + 12, &a.Ipv4.sin_addr.s_addr, 4);
    }
    else if (a.si_family == AF_INET6)
    {
        std::memcpy(out, &a.Ipv6.sin6_addr, 16);
    }
    else
    {
        return;
    }
    const uint16_t p = port(a);
    std::memcpy(out + 16, &p, 2);
}

Net::Address Net::decode(const uint8_t* in)
{
    static constexpr uint8_t kZero[16] = {};
    uint16_t p = 0;
    std::memcpy(&p, in + 16, 2);
    if (std::memcmp(in, kZero, 16) == 0)
    {
        return Address{};
    }
    if (std::memcmp(in, kMappedPrefix, sizeof(kMappedPrefix)) == 0)
    {
        uint32_t v4 = 0;
        std::memcpy(&v4, in + 12, 4);
        return ipv4(v4, p);
    }
    Address a{};
    a.Ipv6.sin6_family = AF_INET6;
    std::memcpy(&a.Ipv6.sin6_addr, in, 16);
    a.Ipv6.sin6_port = htons(p);
    return a;
}
