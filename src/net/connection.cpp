#include "net/connection.h"
#include "game/sacred_addr.h"
#include "config/net.h"
#include "log.h"
#include "patch.h"

#include <winsock2.h>
#include <mstcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
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

    // --- statistics: what TCP went through on each game connection, logged when it closes ---

    std::mutex g_statsMutex;
    std::unordered_map<SOCKET, uint64_t> g_connections;     // sockets TinCat sent on, and since when
    thread_local SOCKET t_lastNoted = INVALID_SOCKET;

    void noteConnection(SOCKET s)
    {
        if (t_lastNoted == s)
        {
            return;
        }
        t_lastNoted = s;
        std::scoped_lock lock(g_statsMutex);
        g_connections.try_emplace(s, GetTickCount64());
    }

    void logStats(SOCKET s)
    {
        uint64_t since = 0;
        {
            std::scoped_lock lock(g_statsMutex);
            const auto it = g_connections.find(s);
            if (it == g_connections.end())
            {
                return;
            }
            since = it->second;
            g_connections.erase(it);
        }
        DWORD version = 0;
        TCP_INFO_v0 info{};
        DWORD bytes = 0;
        // Windows 10 1703 and later; Wine has no such ioctl.
        if (WSAIoctl(s, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &bytes, nullptr, nullptr) != 0)
        {
            return;
        }
        LOG("Game connection (TCP) closed after {} min: RTT {} ms (lowest {}), sent {} KB, resent {} KB, {} "
            "resend time-outs, {} fast resends, received {} KB", (GetTickCount64() - since) / 60000, info.RttUs / 1000,
            info.MinRttUs / 1000, info.BytesOut / 1024, info.BytesRetrans / 1024, info.TimeoutEpisodes,
            info.FastRetrans, info.BytesIn / 1024);
    }

    // --- one send() per message ---

    // TinCat's writer thread sends each message as its 28-byte header and then the payload, as two send() calls:
    // with TCP_NODELAY two segments, and the message is late if either is lost. The header waits for the payload
    // (the next send() of the same thread, right after it) and both go out together.
    constexpr int kHeaderSize = 0x1C;
    constexpr uint32_t kHeaderMagic = 0xDABAFBEF;
    thread_local SOCKET t_heldSocket = INVALID_SOCKET;
    thread_local char t_held[kHeaderSize];
    thread_local std::vector<char> t_joined;

    int sendAll(SOCKET s, const char* buf, int len, int flags)
    {
        int done = 0;
        while (done < len)
        {
            const int n = g_send(s, buf + done, len - done, flags);
            if (n == SOCKET_ERROR)
            {
                return SOCKET_ERROR;
            }
            done += n;
        }
        return done;
    }

    bool isHeader(const char* buf, int len, int flags)
    {
        uint32_t magic = 0, payload = 0;
        if (len != kHeaderSize || flags != 0)
        {
            return false;
        }
        std::memcpy(&magic, buf, 4);
        std::memcpy(&payload, buf + 0x14, 4);
        return magic == kHeaderMagic && payload != 0;
    }

    int WSAAPI hookSend(SOCKET s, const char* buf, int len, int flags)
    {
        if (g_hasDeferred)
        {
            applyDeferred(s);
        }
        noteConnection(s);
        if (t_heldSocket != INVALID_SOCKET)
        {
            const SOCKET held = t_heldSocket;
            t_heldSocket = INVALID_SOCKET;
            if (held == s && flags == 0)
            {
                t_joined.assign(t_held, t_held + kHeaderSize);
                t_joined.insert(t_joined.end(), buf, buf + len);
                const int sent = sendAll(s, t_joined.data(), static_cast<int>(t_joined.size()), 0);
                return sent == SOCKET_ERROR ? SOCKET_ERROR : len;
            }
            sendAll(held, t_held, kHeaderSize, 0);     // not TinCat's pattern after all
        }
        if (isHeader(buf, len, flags))
        {
            std::memcpy(t_held, buf, kHeaderSize);
            t_heldSocket = s;
            return len;
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
        logStats(s);
        if (t_lastNoted == s)
        {
            t_lastNoted = INVALID_SOCKET;
        }
        return g_closesocket(s);
    }

    bool installSendHooks()
    {
        constexpr const char* kTinCat = "tincat2.dll";
        constexpr const char* kWinsock = "WSOCK32.dll";
        g_send = reinterpret_cast<SendFn>(Patch::iat(kTinCat, kWinsock, "send", reinterpret_cast<void*>(&hookSend)));
        if (!g_send)
        {
            return false;
        }
        g_closesocket = reinterpret_cast<CloseSocketFn>(
            Patch::iat(kTinCat, kWinsock, "closesocket", reinterpret_cast<void*>(&hookCloseSocket)));
        if (!g_closesocket)
        {
            Patch::iat(kTinCat, kWinsock, "send", reinterpret_cast<void*>(g_send));
            return false;
        }
        return true;
    }

    void installDeferredNoDelay()
    {
        g_setsockopt = reinterpret_cast<SetSockOptFn>(
            Patch::iat("tincat2.dll", "WSOCK32.dll", "setsockopt", reinterpret_cast<void*>(&hookSetSockOpt)));
    }
}

void Connection::install(bool host)
{
    if (!installSendHooks() || host)
    {
        return;
    }
    installDeferredNoDelay();
    if (!Config::net.noDelay)
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
