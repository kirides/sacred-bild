#include "net/connection.h"
#include "game/sacred_addr.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace
{
    using SetSockOptFn = int(WSAAPI*)(SOCKET, int, int, const char*, int);
    using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
    using CloseSocketFn = int(WSAAPI*)(SOCKET);

    SetSockOptFn g_setsockopt = nullptr;
    SendFn g_send = nullptr;
    CloseSocketFn g_closesocket = nullptr;

    struct Deferred
    {
        SOCKET socket;
        int value;
    };

    // TinCat runs its connections on several threads.
    std::mutex g_mutex;
    std::vector<Deferred> g_deferred;
    std::atomic<bool> g_hasDeferred = false;
    std::atomic<bool> g_logged = false;

    // TinCat (NET_Async_Connect, then KRNL_SendAsyncLogonRequest 0x10004650) sets TCP_NODELAY right after a
    // non-blocking connect. While the handshake is still under way Windows refuses that with WSAEINVAL, and TinCat
    // gives up the join with code -20 (its text: "Kernel: Cannot allocate logdata message"). On a LAN the handshake
    // is usually done by then, over the internet or a VPN it isn't. Nagle's algorithm only holds back sends, so the
    // option is set before the connection's first send instead.
    int WSAAPI hookSetSockOpt(SOCKET s, int level, int name, const char* value, int len)
    {
        const int result = g_setsockopt(s, level, name, value, len);
        if (result == 0 || level != IPPROTO_TCP || name != TCP_NODELAY || !value || len < static_cast<int>(sizeof(int)) ||
            WSAGetLastError() != WSAEINVAL)
        {
            return result;
        }
        const int on = *reinterpret_cast<const int*>(value);
        {
            std::scoped_lock lock(g_mutex);
            std::erase_if(g_deferred, [&](const Deferred& d) { return d.socket == s; });
            g_deferred.push_back({s, on});
            g_hasDeferred = true;
        }
        if (!g_logged.exchange(true))
        {
            LOG("Game connection: TCP_NODELAY refused while connecting, set before the first send instead");
        }
        return 0;
    }

    void applyDeferred(SOCKET s)
    {
        std::scoped_lock lock(g_mutex);
        const auto it = std::ranges::find(g_deferred, s, &Deferred::socket);
        if (it == g_deferred.end())
        {
            return;
        }
        const int err = WSAGetLastError();
        if (g_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&it->value), sizeof(it->value)) != 0 &&
            WSAGetLastError() == WSAEINVAL)
        {
            WSASetLastError(err);
            return;     // still connecting: the send fails as well, try again with the next one
        }
        WSASetLastError(err);
        g_deferred.erase(it);
        g_hasDeferred = !g_deferred.empty();
    }

    int WSAAPI hookSend(SOCKET s, const char* buf, int len, int flags)
    {
        if (g_hasDeferred)
        {
            applyDeferred(s);
        }
        return g_send(s, buf, len, flags);
    }

    int WSAAPI hookCloseSocket(SOCKET s)
    {
        if (g_hasDeferred)
        {
            std::scoped_lock lock(g_mutex);
            std::erase_if(g_deferred, [&](const Deferred& d) { return d.socket == s; });
            g_hasDeferred = !g_deferred.empty();
        }
        return g_closesocket(s);
    }

    void installDeferredNoDelay()
    {
        constexpr const char* kTinCat = "tincat2.dll";
        constexpr const char* kWinsock = "WSOCK32.dll";
        g_send = reinterpret_cast<SendFn>(Patch::iat(kTinCat, kWinsock, "send", reinterpret_cast<void*>(&hookSend)));
        g_closesocket = reinterpret_cast<CloseSocketFn>(
            Patch::iat(kTinCat, kWinsock, "closesocket", reinterpret_cast<void*>(&hookCloseSocket)));
        if (!g_send || !g_closesocket)
        {
            return;
        }
        g_setsockopt = reinterpret_cast<SetSockOptFn>(
            Patch::iat(kTinCat, kWinsock, "setsockopt", reinterpret_cast<void*>(&hookSetSockOpt)));
    }
}

void Connection::install()
{
    installDeferredNoDelay();
    if (!g_config.netNoDelay)
    {
        return;
    }
    // cmp ebx, 2; sete al (after xor eax, eax) -> mov al, 1: drv_disable_nagle is always 1.
    constexpr uint8_t kAlways[] = {0xB0, 0x01, 0x90, 0x90, 0x90, 0x90};
    if (Patch::verify(Sacred::Addr::initNetworkNagleTest, {0x83, 0xFB, 0x02, 0x0F, 0x94, 0xC0}) &&
        Patch::write(Sacred::Addr::initNetworkNagleTest, kAlways, sizeof(kAlways)))
    {
        LOG("Game connection: TCP_NODELAY in both data flow modes");
    }
}
