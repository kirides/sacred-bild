#include "net/tincat_shim.h"
#include "net/matchmaker.h"
#include "net/udp_endpoint.h"
#include "net/udp_transport.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    using SelectFn = int(WSAAPI*)(int, fd_set*, fd_set*, fd_set*, const timeval*);
    using AcceptFn = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
    using ListenFn = int(WSAAPI*)(SOCKET, int);
    using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
    using RecvFn = int(WSAAPI*)(SOCKET, char*, int, int);
    using SetSockOptFn = int(WSAAPI*)(SOCKET, int, int, const char*, int);
    using GetSockOptFn = int(WSAAPI*)(SOCKET, int, int, char*, int*);
    using GetSockNameFn = int(WSAAPI*)(SOCKET, sockaddr*, int*);
    using IoctlFn = int(WSAAPI*)(SOCKET, long, u_long*);
    using ShutdownFn = int(WSAAPI*)(SOCKET, int);
    using CloseSocketFn = int(WSAAPI*)(SOCKET);

    ConnectFn g_connect = nullptr;
    SelectFn g_select = nullptr;
    AcceptFn g_accept = nullptr;
    ListenFn g_listen = nullptr;
    SendFn g_send = nullptr;
    RecvFn g_recv = nullptr;
    SetSockOptFn g_setsockopt = nullptr;
    GetSockOptFn g_getsockopt = nullptr;
    GetSockNameFn g_getsockname = nullptr;
    IoctlFn g_ioctlsocket = nullptr;
    ShutdownFn g_shutdown = nullptr;
    CloseSocketFn g_closesocket = nullptr;

    struct VSock
    {
        UdpTransport::SessionPtr session;
        sockaddr_in target{};       // player: where the game wanted to connect (the TCP fallback)
        bool nonBlocking = false;
        int noDelay = -1;           // TCP_NODELAY TinCat set meanwhile, for the TCP fallback
        bool unreachable = false;   // the handshake failed and there is no TCP fallback (a stand-in address)
    };

    bool g_host = false;
    std::mutex g_mutex;
    std::unordered_map<SOCKET, VSock> g_vsocks;
    std::unordered_set<SOCKET> g_nonBlocking;
    std::atomic<int> g_count = 0;
    std::atomic<SOCKET> g_listener = INVALID_SOCKET;

    std::optional<VSock> find(SOCKET s)
    {
        if (g_count == 0)
        {
            return std::nullopt;
        }
        std::scoped_lock lock(g_mutex);
        const auto it = g_vsocks.find(s);
        return it == g_vsocks.end() ? std::nullopt : std::optional(it->second);
    }

    std::optional<VSock> take(SOCKET s)
    {
        if (g_count == 0)
        {
            return std::nullopt;
        }
        std::scoped_lock lock(g_mutex);
        const auto it = g_vsocks.find(s);
        if (it == g_vsocks.end())
        {
            return std::nullopt;
        }
        VSock v = std::move(it->second);
        g_vsocks.erase(it);
        --g_count;
        return v;
    }

    void add(SOCKET s, VSock v)
    {
        std::scoped_lock lock(g_mutex);
        v.nonBlocking = g_nonBlocking.contains(s);
        g_vsocks[s] = std::move(v);
        ++g_count;
    }

    // The UDP handshake failed: the socket (still unconnected) connects over TCP as the game wanted. A game listed
    // with a stand-in address has no TCP address: the connect fails as on an unreachable network, right away
    // instead of after TCP's time-outs.
    int fallBack(SOCKET s)
    {
        if (const auto v = find(s); v && Matchmaker::isStandIn(v->target.sin_addr.s_addr))
        {
            {
                std::scoped_lock lock(g_mutex);
                if (const auto it = g_vsocks.find(s); it != g_vsocks.end())
                {
                    it->second.unreachable = true;
                }
            }
            UdpTransport::close(v->session);
            WSASetLastError(WSAENETUNREACH);
            return SOCKET_ERROR;
        }
        const auto v = take(s);
        if (!v)
        {
            WSASetLastError(WSAENOTSOCK);
            return SOCKET_ERROR;
        }
        UdpTransport::close(v->session);
        if (v->noDelay >= 0)
        {
            g_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v->noDelay), sizeof(int));
        }
        return g_connect(s, reinterpret_cast<const sockaddr*>(&v->target), sizeof(v->target));
    }

    int WSAAPI hookConnect(SOCKET s, const sockaddr* name, int len)
    {
        if (g_host || !name || len < static_cast<int>(sizeof(sockaddr_in)) || name->sa_family != AF_INET)
        {
            return g_connect(s, name, len);
        }
        const auto& to = *reinterpret_cast<const sockaddr_in*>(name);
        int type = 0;
        int typeLen = sizeof(type);
        if ((ntohl(to.sin_addr.s_addr) >> 24) == 127 ||
            getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen) != 0 || type != SOCK_STREAM)
        {
            return g_connect(s, name, len);
        }
        Matchmaker::Target game{};
        if (Matchmaker::lookup(to.sin_addr.s_addr, ntohs(to.sin_port), game) && !game.udpOk)
        {
            return g_connect(s, name, len);     // a host without [Net] Udp
        }
        auto session = UdpTransport::connect(to);
        if (!session && Matchmaker::isStandIn(to.sin_addr.s_addr))
        {
            WSASetLastError(WSAENETUNREACH);
            return SOCKET_ERROR;
        }
        if (!session)
        {
            return g_connect(s, name, len);
        }
        add(s, {session, to});
        if (find(s)->nonBlocking)
        {
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        return UdpTransport::waitConnected(*session) ? 0 : fallBack(s);
    }

    bool contains(const std::vector<SOCKET>& v, SOCKET s)
    {
        return std::ranges::find(v, s) != v.end();
    }

    std::vector<SOCKET> members(const fd_set* set)
    {
        return set ? std::vector<SOCKET>(set->fd_array, set->fd_array + set->fd_count) : std::vector<SOCKET>();
    }

    void assign(fd_set* set, const std::vector<SOCKET>& v)
    {
        if (set)
        {
            set->fd_count = static_cast<u_int>(v.size());
            std::ranges::copy(v, set->fd_array);
        }
    }

    // select() over a mix of real and virtual sockets: virtual readiness is checked here, the real sockets go to
    // the real select() in slices of at most 10 ms so that a session becoming ready doesn't wait for its time-out.
    int WSAAPI hookSelect(int nfds, fd_set* r, fd_set* w, fd_set* e, const timeval* timeout)
    {
        const SOCKET listener = g_listener;
        const bool listening = listener != INVALID_SOCKET && r && FD_ISSET(listener, r);
        if (g_count == 0 && !listening)
        {
            return g_select(nfds, r, w, e, timeout);
        }
        const auto inR = members(r), inW = members(w), inE = members(e);

        // TinCat's non-blocking connect polls here: a failed UDP handshake turns into the TCP connect.
        std::vector<SOCKET> virt;
        for (const auto* list : {&inR, &inW, &inE})
        {
            for (const SOCKET s : *list)
            {
                if (contains(virt, s))
                {
                    continue;
                }
                if (const auto v = find(s))
                {
                    if (!v->unreachable && UdpTransport::status(*v->session) == UdpTransport::Status::Failed)
                    {
                        fallBack(s);
                    }
                    if (find(s))
                    {
                        virt.push_back(s);      // still virtual: open, connecting, or unreachable
                    }
                }
            }
        }
        fd_set realR{}, realW{}, realE{};
        const auto realOnly = [&](const std::vector<SOCKET>& in, fd_set& out) {
            for (const SOCKET s : in)
            {
                if (!contains(virt, s))
                {
                    out.fd_array[out.fd_count++] = s;
                }
            }
        };
        realOnly(inR, realR);
        realOnly(inW, realW);
        realOnly(inE, realE);
        const bool anyReal = realR.fd_count || realW.fd_count || realE.fd_count;

        const uint64_t start = GetTickCount64();
        const uint64_t limit = timeout ? uint64_t(timeout->tv_sec) * 1000 + timeout->tv_usec / 1000 : UINT64_MAX;
        for (;;)
        {
            const uint64_t seen = UdpTransport::changes();
            std::vector<SOCKET> outR, outW, outE;
            for (const SOCKET s : inR)
            {
                if (contains(virt, s))
                {
                    if (const auto v = find(s); v && !v->unreachable && UdpTransport::readable(*v->session))
                    {
                        outR.push_back(s);
                    }
                }
                else if (s == listener && UdpTransport::hasPendingAccept())
                {
                    outR.push_back(s);
                }
            }
            for (const SOCKET s : inW)
            {
                if (contains(virt, s))
                {
                    if (const auto v = find(s); v && !v->unreachable && UdpTransport::writable(*v->session))
                    {
                        outW.push_back(s);
                    }
                }
            }
            // A failed non-blocking connect shows as an exception; SO_ERROR says why (NET_CheckConnected).
            for (const SOCKET s : inE)
            {
                if (contains(virt, s))
                {
                    if (const auto v = find(s); v && v->unreachable)
                    {
                        outE.push_back(s);
                    }
                }
            }
            const bool virtualReady = !outR.empty() || !outW.empty() || !outE.empty();
            const uint64_t elapsed = GetTickCount64() - start;
            const uint64_t remaining = elapsed >= limit ? 0 : limit - elapsed;
            if (anyReal)
            {
                fd_set cr = realR, cw = realW, ce = realE;
                const uint64_t sliceMs = virtualReady ? 0 : std::min<uint64_t>(remaining, 10);
                const timeval tv{0, static_cast<long>(sliceMs * 1000)};
                const int n = g_select(0, realR.fd_count ? &cr : nullptr, realW.fd_count ? &cw : nullptr,
                    realE.fd_count ? &ce : nullptr, &tv);
                if (n == SOCKET_ERROR)
                {
                    return SOCKET_ERROR;
                }
                const auto merge = [](const fd_set& ready, std::vector<SOCKET>& out) {
                    for (u_int i = 0; i < ready.fd_count; ++i)
                    {
                        if (!contains(out, ready.fd_array[i]))
                        {
                            out.push_back(ready.fd_array[i]);
                        }
                    }
                };
                if (n > 0)
                {
                    merge(cr, outR);
                    merge(cw, outW);
                    merge(ce, outE);
                }
            }
            else if (!virtualReady && remaining > 0)
            {
                UdpTransport::waitForChange(seen, static_cast<uint32_t>(std::min<uint64_t>(remaining, 50)));
            }
            const size_t total = outR.size() + outW.size() + outE.size();
            if (total > 0 || GetTickCount64() - start >= limit)
            {
                assign(r, outR);
                assign(w, outW);
                assign(e, outE);
                return static_cast<int>(total);
            }
        }
    }

    SOCKET WSAAPI hookAccept(SOCKET s, sockaddr* addr, int* addrLen)
    {
        if (s == g_listener && UdpTransport::hasPendingAccept())
        {
            if (auto session = UdpTransport::accept())
            {
                // TinCat needs a socket handle: an unconnected TCP socket nothing else uses stands in.
                const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (handle == INVALID_SOCKET)
                {
                    UdpTransport::close(session);
                    return INVALID_SOCKET;
                }
                const sockaddr_in peer = UdpTransport::peerForGame(*session);
                add(handle, {session, peer});
                if (addr && addrLen && *addrLen >= static_cast<int>(sizeof(peer)))
                {
                    std::memcpy(addr, &peer, sizeof(peer));
                    *addrLen = sizeof(peer);
                }
                return handle;
            }
        }
        return g_accept(s, addr, addrLen);
    }

    int WSAAPI hookListen(SOCKET s, int backlog)
    {
        const int result = g_listen(s, backlog);
        sockaddr_in local{};
        int localLen = sizeof(local);
        if (g_host && result == 0 && getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) == 0)
        {
            g_listener = s;
            UdpEndpoint::open(static_cast<uint16_t>(g_config.netPort), true);
            UdpTransport::setListener(ntohs(local.sin_port));
        }
        return result;
    }

    int WSAAPI hookSend(SOCKET s, const char* buf, int len, int flags)
    {
        if (const auto v = find(s))
        {
            return UdpTransport::send(*v->session, buf, len, v->nonBlocking);
        }
        return g_send(s, buf, len, flags);
    }

    int WSAAPI hookRecv(SOCKET s, char* buf, int len, int flags)
    {
        if (const auto v = find(s))
        {
            return UdpTransport::recv(*v->session, buf, len, v->nonBlocking);
        }
        return g_recv(s, buf, len, flags);
    }

    int WSAAPI hookSetSockOpt(SOCKET s, int level, int name, const char* value, int len)
    {
        if (g_count != 0)
        {
            std::scoped_lock lock(g_mutex);
            if (const auto it = g_vsocks.find(s); it != g_vsocks.end())
            {
                if (level == IPPROTO_TCP && name == TCP_NODELAY && value && len >= static_cast<int>(sizeof(int)))
                {
                    it->second.noDelay = *reinterpret_cast<const int*>(value);
                }
                return 0;
            }
        }
        return g_setsockopt(s, level, name, value, len);
    }

    int WSAAPI hookGetSockOpt(SOCKET s, int level, int name, char* value, int* len)
    {
        const auto v = level == SOL_SOCKET && name == SO_ERROR ? find(s) : std::nullopt;
        if (v && value && len && *len >= static_cast<int>(sizeof(int)))
        {
            *reinterpret_cast<int*>(value) = v->unreachable ? WSAENETUNREACH : 0;
            *len = sizeof(int);
            return 0;
        }
        return g_getsockopt(s, level, name, value, len);
    }

    int WSAAPI hookGetSockName(SOCKET s, sockaddr* name, int* len)
    {
        const auto v = find(s);
        if (!v || !name || !len || *len < static_cast<int>(sizeof(sockaddr_in)))
        {
            return g_getsockname(s, name, len);
        }
        // The local address towards the peer, as a connected TCP socket would report it (over IPv6: 0.0.0.0).
        sockaddr_in local{};
        local.sin_family = AF_INET;
        const Net::Address peerAddress = UdpTransport::peer(*v->session);
        const sockaddr_in& peer = peerAddress.Ipv4;
        const SOCKET probe = Net::isV6(peerAddress) ? INVALID_SOCKET : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe != INVALID_SOCKET)
        {
            int localLen = sizeof(local);
            if (connect(probe, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) == 0)
            {
                getsockname(probe, reinterpret_cast<sockaddr*>(&local), &localLen);
            }
            closesocket(probe);
        }
        local.sin_port = htons(UdpEndpoint::port());
        std::memcpy(name, &local, sizeof(local));
        *len = sizeof(local);
        return 0;
    }

    int WSAAPI hookIoctlSocket(SOCKET s, long cmd, u_long* arg)
    {
        if (cmd == static_cast<long>(FIONBIO) && arg)
        {
            std::scoped_lock lock(g_mutex);
            if (*arg)
            {
                g_nonBlocking.insert(s);
            }
            else
            {
                g_nonBlocking.erase(s);
            }
            if (const auto it = g_vsocks.find(s); it != g_vsocks.end())
            {
                it->second.nonBlocking = *arg != 0;
            }
        }
        return g_ioctlsocket(s, cmd, arg);
    }

    int WSAAPI hookShutdown(SOCKET s, int how)
    {
        if (const auto v = find(s))
        {
            UdpTransport::shutdown(*v->session);
            return 0;
        }
        return g_shutdown(s, how);
    }

    int WSAAPI hookCloseSocket(SOCKET s)
    {
        if (s == g_listener)
        {
            g_listener = INVALID_SOCKET;
            UdpTransport::setListener(0);
        }
        {
            std::scoped_lock lock(g_mutex);
            g_nonBlocking.erase(s);
        }
        if (const auto v = take(s))
        {
            UdpTransport::close(v->session);
        }
        return g_closesocket(s);
    }

    struct Import
    {
        const char* name;
        void* hook;
        void** original;
    };
}

void TincatShim::install(bool host)
{
    if (!g_config.netUdp)
    {
        return;
    }
    g_host = host;
    const Import imports[] = {
        {"connect", reinterpret_cast<void*>(&hookConnect), reinterpret_cast<void**>(&g_connect)},
        {"select", reinterpret_cast<void*>(&hookSelect), reinterpret_cast<void**>(&g_select)},
        {"accept", reinterpret_cast<void*>(&hookAccept), reinterpret_cast<void**>(&g_accept)},
        {"listen", reinterpret_cast<void*>(&hookListen), reinterpret_cast<void**>(&g_listen)},
        {"send", reinterpret_cast<void*>(&hookSend), reinterpret_cast<void**>(&g_send)},
        {"recv", reinterpret_cast<void*>(&hookRecv), reinterpret_cast<void**>(&g_recv)},
        {"setsockopt", reinterpret_cast<void*>(&hookSetSockOpt), reinterpret_cast<void**>(&g_setsockopt)},
        {"getsockopt", reinterpret_cast<void*>(&hookGetSockOpt), reinterpret_cast<void**>(&g_getsockopt)},
        {"getsockname", reinterpret_cast<void*>(&hookGetSockName), reinterpret_cast<void**>(&g_getsockname)},
        {"ioctlsocket", reinterpret_cast<void*>(&hookIoctlSocket), reinterpret_cast<void**>(&g_ioctlsocket)},
        {"shutdown", reinterpret_cast<void*>(&hookShutdown), reinterpret_cast<void**>(&g_shutdown)},
        {"closesocket", reinterpret_cast<void*>(&hookCloseSocket), reinterpret_cast<void**>(&g_closesocket)},
    };
    size_t patched = 0;
    for (; patched < std::size(imports); ++patched)
    {
        const Import& i = imports[patched];
        *i.original = Patch::iat("tincat2.dll", "WSOCK32.dll", i.name, i.hook);
        if (!*i.original)
        {
            break;
        }
    }
    if (patched < std::size(imports))
    {
        // All or nothing: a socket half on UDP would break the connection.
        for (size_t i = 0; i < patched; ++i)
        {
            Patch::iat("tincat2.dll", "WSOCK32.dll", imports[i].name, *imports[i].original);
        }
        LOG("UDP transport off: tincat2.dll's {} import not found", imports[patched].name);
        return;
    }
    if (host)
    {
        LOG("UDP transport: players with [Net] Udp=1 connect over UDP port {}", g_config.netPort);
    }
    else
    {
        LOG("UDP transport: game connections try UDP first (the host's port {} unless the matchmaker knows "
            "another), then TCP", g_config.netPort);
    }
}
