#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

// A UDP address of either family for SacredBild's endpoint, transport and matchmaker client: SOCKADDR_INET holds a
// sockaddr_in or a sockaddr_in6 (si_family says which; 0 = none). Sacred itself only knows IPv4.
namespace Net
{
    using Address = SOCKADDR_INET;

    Address ipv4(uint32_t address, uint16_t port);     // address in network order, port in host order
    Address fromSockaddr(const sockaddr* a);            // AF_INET / AF_INET6; anything else: none
    bool isSet(const Address& a);
    bool isV6(const Address& a);
    bool same(const Address& a, const Address& b);      // family, address and port
    uint16_t port(const Address& a);
    int length(const Address& a);                       // for sendto()
    std::string toString(const Address& a);             // 1.2.3.4:5, [2001:db8::1]:5

    // The protocol's 18-byte form (docs/UDP_PROTOCOL.md): a 16-byte IPv6 address, IPv4 as ::ffff:a.b.c.d, and the
    // port as u16 little-endian; all zero = none.
    constexpr size_t kWireSize = 18;
    void encode(const Address& a, uint8_t* out);
    Address decode(const uint8_t* in);
}
