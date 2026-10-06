#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "log.h"

#include <winsock2.h>
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
    SOCKET g_socket = INVALID_SOCKET;
    WSAEVENT g_readEvent = WSA_INVALID_EVENT;
    HANDLE g_wakeEvent = nullptr;
    uint16_t g_port = 0;

    void dispatch(const uint8_t* data, int size, const sockaddr_in& from)
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

    void run()
    {
        uint8_t buffer[2048];
        uint32_t waitMs = 0;
        for (;;)
        {
            const HANDLE events[] = {g_readEvent, g_wakeEvent};
            WaitForMultipleObjects(2, events, FALSE, waitMs);
            WSAResetEvent(g_readEvent);
            for (;;)
            {
                sockaddr_in from{};
                int fromLen = sizeof(from);
                const int n = recvfrom(g_socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n == SOCKET_ERROR)
                {
                    // WSAEWOULDBLOCK: drained. WSAEMSGSIZE: oversized datagram, dropped. Others: try again later.
                    if (WSAGetLastError() == WSAEMSGSIZE)
                    {
                        continue;
                    }
                    break;
                }
                if (from.sin_family == AF_INET)
                {
                    dispatch(buffer, n, from);
                }
            }
            const uint64_t now = UdpProto::nowMs();
            waitMs = 1000;
            for (const auto tick : g_ticks)
            {
                waitMs = std::min(waitMs, tick(now));
            }
        }
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
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        LOG("UDP endpoint: socket failed: {}", WSAGetLastError());
        return false;
    }
    if (broadcast)
    {
        BOOL on = TRUE;
        setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    // Otherwise an ICMP "port unreachable" from a peer that is gone fails the next recvfrom.
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(g_socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
    // Room for bursts while the I/O thread is busy (a player loading in receives a lot at once).
    int size = 1 << 20;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(g_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (bind(g_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        LOG("UDP endpoint: port {} is not available ({})", port, WSAGetLastError());
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return false;
    }
    int localLen = sizeof(local);
    getsockname(g_socket, reinterpret_cast<sockaddr*>(&local), &localLen);
    g_port = ntohs(local.sin_port);

    g_readEvent = WSACreateEvent();
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Makes the socket non-blocking as well.
    WSAEventSelect(g_socket, g_readEvent, FD_READ);
    std::thread(run).detach();
    g_open = true;
    LOG("UDP endpoint: port {}", g_port);
    return true;
}

bool UdpEndpoint::isOpen()
{
    return g_open;
}

uint16_t UdpEndpoint::port()
{
    return g_port;
}

bool UdpEndpoint::sendTo(const void* data, int size, const sockaddr_in& to)
{
    if (!g_open)
    {
        return false;
    }
    return sendto(g_socket, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&to),
        sizeof(to)) == size;
}

void UdpEndpoint::wake()
{
    if (g_wakeEvent)
    {
        SetEvent(g_wakeEvent);
    }
}
