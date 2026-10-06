#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    struct Route
    {
        char protocol[3];
        UdpEndpoint::Handler handler;
    };

    std::vector<Route> g_routes;
    std::vector<UdpEndpoint::Tick> g_ticks;

    std::mutex g_openMutex;
    bool g_tried = false;
    std::atomic<bool> g_open = false;
    SOCKET g_socket4 = INVALID_SOCKET;
    SOCKET g_socket6 = INVALID_SOCKET;      // where the system has IPv6
    WSAEVENT g_readEvent = WSA_INVALID_EVENT;
    HANDLE g_wakeEvent = nullptr;
    uint16_t g_port = 0;
    uint16_t g_port6 = 0;
    std::atomic<int> g_dropFamily = 0;

    void dispatch(const uint8_t* data, int size, const Net::Address& from)
    {
        if (size < 4)
        {
            return;
        }
        for (const Route& r : g_routes)
        {
            if (std::memcmp(data, r.protocol, 3) == 0)
            {
                r.handler(data, size, from);
                return;
            }
        }
    }

    void drain(SOCKET s, uint8_t* buffer, int size)
    {
        if (s == INVALID_SOCKET)
        {
            return;
        }
        for (;;)
        {
            sockaddr_storage from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(s, reinterpret_cast<char*>(buffer), size, 0, reinterpret_cast<sockaddr*>(&from),
                &fromLen);
            if (n == SOCKET_ERROR)
            {
                // WSAEWOULDBLOCK: drained. WSAEMSGSIZE: oversized datagram, dropped. Others: try again later.
                if (WSAGetLastError() == WSAEMSGSIZE)
                {
                    continue;
                }
                return;
            }
            const Net::Address address = Net::fromSockaddr(reinterpret_cast<const sockaddr*>(&from));
            const bool dropped = address.si_family == g_dropFamily && n >= 3 &&
                std::memcmp(buffer, UdpProto::kTransport, 3) == 0;
            if (Net::isSet(address) && !dropped)
            {
                dispatch(buffer, n, address);
            }
        }
    }

    void run()
    {
        uint8_t buffer[2048];
        uint32_t waitMs = 0;
        for (;;)
        {
            const HANDLE events[] = {g_readEvent, g_wakeEvent};
            WaitForMultipleObjects(2, events, FALSE, waitMs);
            WSAResetEvent(g_readEvent);
            drain(g_socket4, buffer, sizeof(buffer));
            drain(g_socket6, buffer, sizeof(buffer));
            const uint64_t now = UdpProto::nowMs();
            waitMs = 1000;
            for (const auto tick : g_ticks)
            {
                waitMs = std::min(waitMs, tick(now));
            }
        }
    }

    SOCKET openSocket(int family, uint16_t port, bool broadcast, uint16_t& bound)
    {
        const SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            return s;
        }
        if (family == AF_INET6)
        {
            // IPv4 has its own socket: this one only IPv6 (no IPv4-mapped addresses).
            DWORD only = 1;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only));
        }
        if (broadcast && family == AF_INET)
        {
            BOOL on = TRUE;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        }
        // Otherwise an ICMP "port unreachable" from a peer that is gone fails the next recvfrom.
        BOOL report = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
        // Room for bursts while the I/O thread is busy (a player loading in receives a lot at once).
        int size = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));

        sockaddr_storage local{};
        int localLen;
        if (family == AF_INET)
        {
            auto& a = reinterpret_cast<sockaddr_in&>(local);
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            localLen = sizeof(a);
        }
        else
        {
            auto& a = reinterpret_cast<sockaddr_in6&>(local);
            a.sin6_family = AF_INET6;
            a.sin6_port = htons(port);
            localLen = sizeof(a);
        }
        if (bind(s, reinterpret_cast<const sockaddr*>(&local), localLen) == SOCKET_ERROR)
        {
            LOG("UDP endpoint: {} port {} is not available ({})", family == AF_INET ? "IPv4" : "IPv6", port,
                WSAGetLastError());
            closesocket(s);
            return INVALID_SOCKET;
        }
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen);
        bound = Net::port(Net::fromSockaddr(reinterpret_cast<const sockaddr*>(&local)));
        // Makes the socket non-blocking as well.
        WSAEventSelect(s, g_readEvent, FD_READ);
        return s;
    }
}

void UdpEndpoint::onPacket(const char (&protocol)[3], Handler handler)
{
    Route r{};
    std::memcpy(r.protocol, protocol, 3);
    r.handler = handler;
    g_routes.push_back(r);
}

void UdpEndpoint::onTick(Tick tick)
{
    g_ticks.push_back(tick);
}

bool UdpEndpoint::open(uint16_t port, bool broadcast)
{
    if (g_open)
    {
        return true;
    }
    std::scoped_lock lock(g_openMutex);
    if (g_tried)
    {
        return g_open;
    }
    g_tried = true;
    g_readEvent = WSACreateEvent();
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_socket4 = openSocket(AF_INET, port, broadcast, g_port);
    if (g_socket4 == INVALID_SOCKET)
    {
        LOG("UDP endpoint: no IPv4 socket ({})", WSAGetLastError());
        return false;
    }
    // The same port for IPv6 (a player's ephemeral IPv4 port may be taken there: any free one then).
    g_socket6 = openSocket(AF_INET6, port ? port : g_port, false, g_port6);
    if (g_socket6 == INVALID_SOCKET && !port)
    {
        g_socket6 = openSocket(AF_INET6, 0, false, g_port6);
    }
    std::thread(run).detach();
    g_open = true;
    if (g_socket6 != INVALID_SOCKET)
    {
        LOG("UDP endpoint: port {} (IPv4), {} (IPv6)", g_port, g_port6);
    }
    else
    {
        LOG("UDP endpoint: port {} (IPv4; no IPv6)", g_port);
    }
    return true;
}

bool UdpEndpoint::isOpen()
{
    return g_open;
}

bool UdpEndpoint::hasIpv6()
{
    return g_socket6 != INVALID_SOCKET;
}

uint16_t UdpEndpoint::port()
{
    return g_port;
}

bool UdpEndpoint::sendTo(const void* data, int size, const Net::Address& to)
{
    if (!g_open)
    {
        return false;
    }
    const SOCKET s = Net::isV6(to) ? g_socket6 : g_socket4;
    if (s == INVALID_SOCKET || !Net::isSet(to))
    {
        return false;
    }
    return sendto(s, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&to),
        Net::length(to)) == size;
}

void UdpEndpoint::wake()
{
    if (g_wakeEvent)
    {
        SetEvent(g_wakeEvent);
    }
}

void UdpEndpoint::dropReceived(int family)
{
    g_dropFamily = family;
}
