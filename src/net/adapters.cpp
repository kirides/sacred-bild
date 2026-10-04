#include "net/adapters.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

std::vector<Net::LocalAddress> Net::localAddresses()
{
    std::vector<LocalAddress> out;
    std::vector<uint8_t> buffer(16 * 1024);
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
        GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = static_cast<ULONG>(buffer.size());
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG err = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (err == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        err = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (err != NO_ERROR)
    {
        return out;
    }
    for (const auto* a = adapters; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }
        for (const auto* u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (u->Address.lpSockaddr->sa_family != AF_INET || u->DadState != IpDadStatePreferred)
            {
                continue;
            }
            const uint32_t address = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            const unsigned prefix = u->OnLinkPrefixLength;
            uint32_t broadcast = 0;
            if (prefix > 0 && prefix <= 30)
            {
                broadcast = address | ~htonl(0xFFFFFFFFu << (32 - prefix));
            }
            out.push_back({a->IfIndex, address, broadcast});
        }
    }
    return out;
}

uint32_t Net::limitedBroadcastInterface()
{
    DWORD index = 0;
    return GetBestInterface(INADDR_BROADCAST, &index) == NO_ERROR ? index : 0;
}

std::string Net::toString(uint32_t address)
{
    in_addr a{};
    a.s_addr = address;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}
