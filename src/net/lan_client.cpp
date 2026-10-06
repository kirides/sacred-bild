#include "net/lan_client.h"
#include "net/adapters.h"
#include "net/lan_protocol.h"
#include "net/matchmaker.h"
#include "game/sacred_addr.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <detours/detours.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace Sacred;
    using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
        LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
    using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
    using FdIsSetFn = int(WSAAPI*)(SOCKET, fd_set*);

    CreateProcessAFn g_createProcess = nullptr;
    RecvFromFn g_recvfrom = nullptr;
    FdIsSetFn g_fdIsSet = nullptr;

    BOOL WINAPI hookCreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES processAttr,
        LPSECURITY_ATTRIBUTES threadAttr, BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA startup,
        LPPROCESS_INFORMATION info)
    {
        const char* name = app ? std::strrchr(app, '\\') : nullptr;
        if (app && _stricmp(name ? name + 1 : app, "GameServer.exe") == 0)
        {
            HMODULE self = nullptr;
            char dll[MAX_PATH] = {};
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&hookCreateProcessA), &self);
            GetModuleFileNameA(self, dll, MAX_PATH);
            if (DetourCreateProcessWithDllExA(app, cmd, processAttr, threadAttr, inherit, flags, env, dir, startup, info,
                    dll, g_createProcess))
            {
                LOG("Started {} with SacredBild (see SacredBild-server.log)", app);
                return TRUE;
            }
            LOG("Starting {} with SacredBild failed ({}); starting it without", app, GetLastError());
        }
        return g_createProcess(app, cmd, processAttr, threadAttr, inherit, flags, env, dir, startup, info);
    }

    // --- [Net] Hosts ---

    struct Host
    {
        std::string name;
        uint16_t port;
    };

    constexpr uint64_t kResolveMs = 30000;

    std::vector<Host> g_hosts;
    std::mutex g_targetsMutex;
    std::vector<sockaddr_in> g_targets;     // resolved hosts, this PC's own addresses left out
    std::atomic<bool> g_resolving = false;
    uint64_t g_lastResolve = 0;
    std::string g_resolveLog;               // resolver thread only

    // The poll's state; the poll runs on one thread.
    SOCKET g_socket = INVALID_SOCKET;
    bool g_socketFailed = false;
    uint64_t g_lastSubscribe = 0;
    struct Pending
    {
        uint8_t wire[LanAnnounce::kWireSize];
        sockaddr_in from;
    };
    std::deque<Pending> g_pending;      // announcements the poll's next recvfrom calls return
    constexpr size_t kMaxPending = 64;
    std::vector<uint32_t> g_heardFrom;
    std::vector<Net::LocalAddress> g_localAddresses;
    uint64_t g_localAddressesTime = 0;

    std::vector<Host> parseHosts(const std::string& list)
    {
        std::vector<Host> out;
        size_t pos = 0;
        while (pos < list.size())
        {
            size_t end = list.find_first_of(",; ", pos);
            if (end == std::string::npos)
            {
                end = list.size();
            }
            std::string item = list.substr(pos, end - pos);
            pos = end + 1;
            int port = g_config.netPort;
            if (const size_t colon = item.rfind(':'); colon != std::string::npos)
            {
                port = std::atoi(item.c_str() + colon + 1);
                item.resize(colon);
            }
            if (!item.empty() && port > 0 && port < 0x10000)
            {
                out.push_back({item, static_cast<uint16_t>(port)});
            }
        }
        return out;
    }

    std::vector<sockaddr_in> resolveHosts(std::string& log)
    {
        const auto local = Net::localAddresses();
        std::vector<sockaddr_in> out;
        for (const Host& host : g_hosts)
        {
            log += log.empty() ? "" : ", ";
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(host.name.c_str(), nullptr, &hints, &result) != 0 || !result)
            {
                log += host.name + " (not found)";
                continue;
            }
            sockaddr_in addr = *reinterpret_cast<const sockaddr_in*>(result->ai_addr);
            freeaddrinfo(result);
            addr.sin_port = htons(host.port);
            const std::string text = Net::toString(addr.sin_addr.s_addr);
            log += std::format("{}{}:{}", host.name == text ? "" : host.name + " = ", text, host.port);
            // This PC's own games arrive by broadcast.
            if (std::ranges::find(local, addr.sin_addr.s_addr, &Net::LocalAddress::address) != local.end())
            {
                log += " (this PC, skipped)";
                continue;
            }
            out.push_back(addr);
        }
        return out;
    }

    void startResolve(uint64_t now)
    {
        if (g_resolving.exchange(true))
        {
            return;
        }
        g_lastResolve = now;
        // Host names can take a while to resolve; the poll runs on the UI thread.
        std::thread([] {
            std::string log;
            auto targets = resolveHosts(log);
            if (log != g_resolveLog)
            {
                g_resolveLog = log;
                LOG("LAN list: asking {} for their games", log);
            }
            {
                std::scoped_lock lock(g_targetsMutex);
                g_targets = std::move(targets);
            }
            g_resolving = false;
        }).detach();
    }

    SOCKET lanSocket()
    {
        const auto* client = *reinterpret_cast<const uint8_t* const*>(Addr::g_pGameClient);
        return client ? *reinterpret_cast<const SOCKET*>(client + Addr::gameClient_lanSocket) : INVALID_SOCKET;
    }

    void openSocket()
    {
        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_socket == INVALID_SOCKET)
        {
            g_socketFailed = true;
            LOG("LAN list: socket failed: {}", WSAGetLastError());
            return;
        }
        u_long nonBlocking = 1;
        ioctlsocket(g_socket, FIONBIO, &nonBlocking);
        // Otherwise an ICMP "port unreachable" from a host without a gameserver fails the next recvfrom.
        BOOL report = FALSE;
        DWORD bytes = 0;
        WSAIoctl(g_socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (bind(g_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            LOG("LAN list: bind failed: {}", WSAGetLastError());
        }
    }

    void pollMatchmaker()
    {
        std::vector<Matchmaker::Listed> games;
        Matchmaker::poll(games);
        for (const auto& g : games)
        {
            if (g_pending.size() >= kMaxPending)
            {
                break;
            }
            // Joining connects to the address in the announcement: the host's as the matchmaker sees it, or a
            // stand-in the transport maps back to the game.
            Pending p{};
            LanAnnounce::encode(g.plain, g.address, p.wire);
            p.from.sin_family = AF_INET;
            p.from.sin_addr.s_addr = g.address;
            std::memcpy(&p.from.sin_port, g.plain + 2, 2);
            p.from.sin_port = htons(p.from.sin_port);
            g_pending.push_back(p);
        }
    }

    // Renews the subscriptions and queues the announcements from hosts and from the matchmaker.
    void poll()
    {
        pollMatchmaker();
        if (g_hosts.empty())
        {
            return;
        }
        const uint64_t now = GetTickCount64();
        if (g_lastResolve == 0 || now - g_lastResolve >= kResolveMs)
        {
            startResolve(now);
        }
        if (g_socket == INVALID_SOCKET)
        {
            if (g_socketFailed)
            {
                return;
            }
            openSocket();
            if (g_socket == INVALID_SOCKET)
            {
                return;
            }
        }
        std::vector<sockaddr_in> targets;
        {
            std::scoped_lock lock(g_targetsMutex);
            targets = g_targets;
        }
        if (now - g_lastSubscribe >= LanRelay::kResubscribeMs)
        {
            g_lastSubscribe = now;
            uint8_t msg[LanRelay::kSubscribeSize] = {};
            LanRelay::writeHeader(msg, LanRelay::kSubscribeMagic);
            for (const auto& t : targets)
            {
                sendto(g_socket, reinterpret_cast<const char*>(msg), sizeof(msg), 0,
                    reinterpret_cast<const sockaddr*>(&t), sizeof(t));
            }
        }

        uint8_t msg[512];
        while (g_pending.size() < kMaxPending)
        {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(g_socket, reinterpret_cast<char*>(msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR)
            {
                break;
            }
            const bool known = std::ranges::find(targets, from.sin_addr.s_addr,
                [](const sockaddr_in& t) { return t.sin_addr.s_addr; }) != targets.end();
            if (!known || !LanRelay::hasHeader(msg, n, LanRelay::kAnnounceMagic, LanRelay::kAnnounceSize))
            {
                continue;
            }
            // Joining connects to the address in the announcement: the one this host answered from.
            Pending p{};
            LanAnnounce::encode(msg + 8, from.sin_addr.s_addr, p.wire);
            p.from = from;
            g_pending.push_back(p);
            if (std::ranges::find(g_heardFrom, from.sin_addr.s_addr) == g_heardFrom.end())
            {
                g_heardFrom.push_back(from.sin_addr.s_addr);
                LOG("LAN list: game '{}' from {}", LanAnnounce::gameName(msg + 8), Net::toString(from.sin_addr.s_addr));
            }
        }
    }

    // A copy of an announcement that this PC's own relay sent on another adapter: the game's own broadcast lists
    // the game here already.
    bool fromOwnRelay(const sockaddr_in& from)
    {
        if (from.sin_family != AF_INET || from.sin_port != htons(static_cast<u_short>(g_config.netPort)))
        {
            return false;
        }
        const uint64_t now = GetTickCount64();
        if (g_localAddressesTime == 0 || now - g_localAddressesTime >= 10000)
        {
            g_localAddressesTime = now;
            g_localAddresses = Net::localAddresses();
        }
        return std::ranges::find(g_localAddresses, from.sin_addr.s_addr, &Net::LocalAddress::address) !=
            g_localAddresses.end();
    }

    int WSAAPI hookFdIsSet(SOCKET s, fd_set* set)
    {
        if (s == lanSocket())
        {
            poll();
            if (!g_pending.empty())
            {
                return 1;
            }
        }
        return g_fdIsSet(s, set);
    }

    int WSAAPI hookRecvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromLen)
    {
        if (s != lanSocket())
        {
            return g_recvfrom(s, buf, len, flags, from, fromLen);
        }
        if (!g_pending.empty() && len >= static_cast<int>(LanAnnounce::kWireSize))
        {
            const Pending& p = g_pending.front();
            std::memcpy(buf, p.wire, sizeof(p.wire));
            if (from && fromLen && *fromLen >= static_cast<int>(sizeof(p.from)))
            {
                std::memcpy(from, &p.from, sizeof(p.from));
                *fromLen = sizeof(p.from);
            }
            g_pending.pop_front();
            return static_cast<int>(LanAnnounce::kWireSize);
        }
        sockaddr_in sender{};
        int senderLen = sizeof(sender);
        const int n = g_recvfrom(s, buf, len, flags, reinterpret_cast<sockaddr*>(&sender), &senderLen);
        if (n != SOCKET_ERROR && g_config.netRelay && fromOwnRelay(sender))
        {
            WSASetLastError(WSAEWOULDBLOCK);    // the poll skips it and selects again
            return SOCKET_ERROR;
        }
        if (n != SOCKET_ERROR && from && fromLen)
        {
            std::memcpy(from, &sender, std::min(*fromLen, senderLen));
            *fromLen = senderLen;
        }
        return n;
    }
}

void LanClient::install()
{
    // The gameserver needs SacredBild for the relay, the UDP transport and the matchmaker.
    if (g_config.netRelay || g_config.netUdp || Matchmaker::enabled())
    {
        g_createProcess = reinterpret_cast<CreateProcessAFn>(
            Patch::iat("KERNEL32.dll", "CreateProcessA", reinterpret_cast<void*>(&hookCreateProcessA)));
        if (g_createProcess)
        {
            LOG("Hosted games start their gameserver with SacredBild");
        }
    }

    g_hosts = parseHosts(g_config.netHosts);
    const bool listMore = !g_hosts.empty() || Matchmaker::enabled();
    if (!g_config.netRelay && !listMore)
    {
        return;
    }
    g_recvfrom = reinterpret_cast<RecvFromFn>(Patch::iat("WS2_32.dll", "recvfrom", reinterpret_cast<void*>(&hookRecvfrom)));
    if (!g_recvfrom || !listMore)
    {
        return;
    }
    g_fdIsSet = reinterpret_cast<FdIsSetFn>(Patch::iat("WS2_32.dll", "__WSAFDIsSet", reinterpret_cast<void*>(&hookFdIsSet)));
    if (g_fdIsSet && !g_hosts.empty())
    {
        LOG("LAN list: also lists the games of {} host(s) from [Net] Hosts", g_hosts.size());
    }
    if (g_fdIsSet && Matchmaker::enabled())
    {
        LOG("LAN list: also lists the games of the matchmaker {}", g_config.netMatchmaker);
    }
}

uint16_t LanClient::relayPort(uint32_t address)
{
    {
        std::scoped_lock lock(g_targetsMutex);
        for (const auto& t : g_targets)
        {
            if (t.sin_addr.s_addr == address)
            {
                return ntohs(t.sin_port);
            }
        }
    }
    return static_cast<uint16_t>(g_config.netPort);
}
