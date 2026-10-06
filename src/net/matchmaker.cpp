#include "net/matchmaker.h"
#include "net/adapters.h"
#include "net/lan_protocol.h"
#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "config.h"
#include "log.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace UdpProto;

namespace
{
    constexpr uint16_t kDefaultPort = 2107;
    constexpr uint64_t kResolveMs = 10 * 60000;
    constexpr uint64_t kRetryResolveMs = 15000;
    constexpr uint32_t kRegisterMs = 5000;      // until the matchmaker says otherwise
    constexpr uint64_t kListMs = 2000;
    constexpr uint64_t kAnnounceMs = 2000;      // Sacred drops a LAN game after 5 s without an announcement
    constexpr uint64_t kGameExpiryMs = 10000;
    constexpr uint64_t kJoinMinMs = 500;
    constexpr uint16_t kMaxPages = 20;
    constexpr uint64_t kPunchDelaysMs[] = {0, 100, 300};

    bool g_host = false;

    // Echoed in the answers; not at DLL load (the OS's generator may load a DLL).
    uint32_t nonce()
    {
        static const uint32_t value = random32() | 1;
        return value;
    }

    std::mutex g_mutex;

    // The matchmaker's address.
    std::string g_name;
    uint16_t g_port = kDefaultPort;
    sockaddr_in g_server{};
    bool g_resolved = false;
    uint64_t g_resolvedAt = 0;
    std::atomic<bool> g_resolving = false;
    uint8_t g_cookie[kCookieSize]{};
    bool g_hasCookie = false;

    // Host.
    uint8_t g_plain[LanAnnounce::kSize]{};
    bool g_hasGame = false;
    uint64_t g_nextRegister = 0;
    uint32_t g_refreshMs = kRegisterMs;
    uint32_t g_gameId = 0;
    sockaddr_in g_public{};
    struct Punch
    {
        sockaddr_in to;
        uint32_t gameId;
        uint64_t at;
    };
    std::vector<Punch> g_punches;

    // Player.
    struct Game
    {
        uint32_t id;
        sockaddr_in udp;
        uint16_t flags;
        uint8_t plain[LanAnnounce::kSize];
        uint64_t seen;
    };
    std::vector<Game> g_games;
    uint16_t g_pageCount = 1;
    uint64_t g_nextList = 0;
    uint64_t g_lastAnnounced = 0;
    size_t g_loggedCount = SIZE_MAX;
    uint32_t g_pendingJoin = 0;
    uint64_t g_lastJoin = 0;

    std::string endpoint(const sockaddr_in& a)
    {
        return std::format("{}:{}", Net::toString(a.sin_addr.s_addr), ntohs(a.sin_port));
    }

    sockaddr_in address(const uint8_t* ip, uint16_t port)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        std::memcpy(&a.sin_addr.s_addr, ip, 4);
        a.sin_port = htons(port);
        return a;
    }

    void parseServer()
    {
        std::string text = g_config.netMatchmaker;
        g_port = kDefaultPort;
        if (const size_t colon = text.rfind(':'); colon != std::string::npos)
        {
            const int port = std::atoi(text.c_str() + colon + 1);
            if (port > 0 && port < 0x10000)
            {
                g_port = static_cast<uint16_t>(port);
            }
            text.resize(colon);
        }
        g_name = text;
    }

    // Names can take a while to resolve; neither the I/O thread nor the UI thread waits for it.
    void resolve(uint64_t now)
    {
        if (g_resolving.exchange(true))
        {
            return;
        }
        g_resolvedAt = now;
        std::thread([] {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            sockaddr_in server{};
            const bool ok = getaddrinfo(g_name.c_str(), nullptr, &hints, &result) == 0 && result;
            if (ok)
            {
                server = *reinterpret_cast<const sockaddr_in*>(result->ai_addr);
                server.sin_port = htons(g_port);
                freeaddrinfo(result);
            }
            {
                std::scoped_lock lock(g_mutex);
                const bool changed = ok && (!g_resolved || server.sin_addr.s_addr != g_server.sin_addr.s_addr);
                if (ok)
                {
                    g_server = server;
                    g_resolved = true;
                }
                if (changed)
                {
                    LOG("Matchmaker: {} = {}", g_name, endpoint(server));
                    g_hasCookie = false;
                }
                else if (!ok && !g_resolved)
                {
                    LOG("Matchmaker: {} not found", g_name);
                    g_resolvedAt -= kResolveMs - kRetryResolveMs;
                }
            }
            g_resolving = false;
            UdpEndpoint::wake();
        }).detach();
    }

    // g_mutex held.
    void send(uint8_t* p, size_t size)
    {
        if (g_resolved)
        {
            UdpEndpoint::sendTo(p, static_cast<int>(size), g_server);
        }
    }

    // A request with the nonce and the cookie; g_mutex held.
    void request(uint8_t* p, size_t size, char type)
    {
        magic(p, kMatchmaker, type);
        put<uint32_t>(p, 4, kVersion);
        put<uint32_t>(p, 8, nonce());
        if (g_hasCookie)
        {
            std::memcpy(p + 16, g_cookie, kCookieSize);
        }
        send(p, size);
    }

    void sendRegister()
    {
        uint8_t p[M::kRegisterSize] = {};
        put<uint16_t>(p, 12, g_config.netUdp ? M::kFlagUdp : 0);
        std::memcpy(p + 32, g_plain, LanAnnounce::kSize);
        request(p, sizeof(p), M::Register);
    }

    void sendList(uint16_t page)
    {
        uint8_t p[M::kListSize] = {};
        put<uint16_t>(p, 12, page);
        request(p, sizeof(p), M::List);
    }

    void sendJoin(uint32_t gameId)
    {
        uint8_t p[M::kJoinSize] = {};
        put<uint32_t>(p, 12, gameId);
        request(p, sizeof(p), M::Join);
    }

    void onPacket(const uint8_t* p, int n, const sockaddr_in& from)
    {
        if (n < 12 || get<uint32_t>(p, 4) != kVersion)
        {
            return;
        }
        std::scoped_lock lock(g_mutex);
        if (!g_resolved || from.sin_addr.s_addr != g_server.sin_addr.s_addr || from.sin_port != g_server.sin_port)
        {
            return;
        }
        const char type = static_cast<char>(p[3]);
        const uint64_t now = nowMs();
        if (type == M::Introduce)
        {
            // Not an answer: carries no nonce. Only the matchmaker's address is trusted for it.
            if (g_host && n >= static_cast<int>(M::kIntroduceSize) && g_gameId && get<uint32_t>(p, 8) == g_gameId)
            {
                const sockaddr_in player = address(p + 12, get<uint16_t>(p, 16));
                LOG("Matchmaker: a player at {} joins, opening the way", endpoint(player));
                for (const uint64_t delay : kPunchDelaysMs)
                {
                    g_punches.push_back({player, g_gameId, now + delay});
                }
                UdpEndpoint::wake();
            }
            return;
        }
        if (get<uint32_t>(p, 8) != nonce())
        {
            return;
        }
        switch (type)
        {
        case M::Challenge:
            if (n >= static_cast<int>(M::kChallengeSize))
            {
                std::memcpy(g_cookie, p + 16, kCookieSize);
                g_hasCookie = true;
                if (g_host && g_hasGame)
                {
                    sendRegister();
                }
                if (!g_host)
                {
                    g_nextList = 0;
                    if (g_pendingJoin)
                    {
                        sendJoin(g_pendingJoin);
                    }
                }
                UdpEndpoint::wake();
            }
            break;
        case M::Registered:
            if (g_host && n >= static_cast<int>(M::kRegisteredSize))
            {
                const uint32_t id = get<uint32_t>(p, 12);
                g_refreshMs = std::clamp(get<uint32_t>(p, 16), 1000u, 30000u);
                const sockaddr_in seen = address(p + 20, get<uint16_t>(p, 24));
                if (id != g_gameId || seen.sin_addr.s_addr != g_public.sin_addr.s_addr || seen.sin_port != g_public.sin_port)
                {
                    LOG("Matchmaker: game '{}' listed (id {:08x}); players reach this gameserver at {}",
                        LanAnnounce::gameName(g_plain), id, endpoint(seen));
                }
                g_gameId = id;
                g_public = seen;
            }
            break;
        case M::Games:
            if (!g_host && n >= static_cast<int>(M::kGamesHeader))
            {
                g_pageCount = std::clamp<uint16_t>(get<uint16_t>(p, 14), 1, kMaxPages);
                const size_t count = std::min<size_t>(get<uint16_t>(p, 16), (n - M::kGamesHeader) / M::kGameEntry);
                for (size_t i = 0; i < count; ++i)
                {
                    const uint8_t* e = p + M::kGamesHeader + i * M::kGameEntry;
                    Game g{};
                    g.id = get<uint32_t>(e, 0);
                    g.udp = address(e + 4, get<uint16_t>(e, 8));
                    g.flags = get<uint16_t>(e, 10);
                    std::memcpy(g.plain, e + 12, LanAnnounce::kSize);
                    g.seen = now;
                    const auto it = std::ranges::find(g_games, g.id, &Game::id);
                    if (it != g_games.end())
                    {
                        *it = g;
                    }
                    else
                    {
                        g_games.push_back(g);
                    }
                }
            }
            break;
        case M::Joined:
            if (!g_host && n >= static_cast<int>(M::kJoinedSize))
            {
                const uint32_t id = get<uint32_t>(p, 12);
                const auto it = std::ranges::find(g_games, id, &Game::id);
                if (get<uint32_t>(p, 16) == 0)
                {
                    LOG("Matchmaker: game {:08x} is gone", id);
                }
                else if (it != g_games.end())
                {
                    it->udp = address(p + 16, get<uint16_t>(p, 20));
                    it->flags = get<uint16_t>(p, 22);
                }
                if (g_pendingJoin == id)
                {
                    g_pendingJoin = 0;
                }
            }
            break;
        default:
            break;
        }
    }

    uint32_t tick(uint64_t now)
    {
        std::scoped_lock lock(g_mutex);
        if (g_resolvedAt == 0 || now - g_resolvedAt >= kResolveMs)
        {
            resolve(now);
        }
        uint32_t wait = 1000;
        if (g_host && g_hasGame && g_resolved)
        {
            if (now >= g_nextRegister)
            {
                sendRegister();
                g_nextRegister = now + g_refreshMs;
            }
            wait = static_cast<uint32_t>(std::min<uint64_t>(wait, g_nextRegister - now));
        }
        std::erase_if(g_punches, [&](const Punch& punch) {
            if (now < punch.at)
            {
                wait = static_cast<uint32_t>(std::min<uint64_t>(wait, punch.at - now));
                return false;
            }
            uint8_t p[T::kHeaderSize] = {};
            magic(p, kTransport, T::Punch);
            put<uint32_t>(p, 4, punch.gameId);
            UdpEndpoint::sendTo(p, sizeof(p), punch.to);
            return true;
        });
        return std::max<uint32_t>(wait, 1);
    }
}

bool Matchmaker::enabled()
{
    return !g_config.netMatchmaker.empty();
}

void Matchmaker::install(bool host)
{
    if (!enabled())
    {
        return;
    }
    g_host = host;
    parseServer();
    UdpEndpoint::onPacket(kMatchmaker, onPacket);
    UdpEndpoint::onTick(tick);
    if (host && !g_config.netPublish)
    {
        LOG("Matchmaker: hosted games are not published ([Net] Publish=0)");
    }
}

void Matchmaker::publish(const uint8_t* plain)
{
    if (!enabled() || !g_host || !g_config.netPublish)
    {
        return;
    }
    std::scoped_lock lock(g_mutex);
    const bool changed = !g_hasGame || std::memcmp(g_plain + 8, plain + 8, LanAnnounce::kSize - 8) != 0;
    std::memcpy(g_plain, plain, LanAnnounce::kSize);
    if (!g_hasGame)
    {
        LOG("Matchmaker: publishing '{}' at {}:{}", LanAnnounce::gameName(plain), g_name, g_port);
    }
    g_hasGame = true;
    if (changed)
    {
        g_nextRegister = 0;     // players and name show up right away
        UdpEndpoint::wake();
    }
}

void Matchmaker::poll(std::vector<Listed>& out)
{
    if (!enabled() || g_host || !UdpEndpoint::open(0, false))
    {
        return;
    }
    const uint64_t now = nowMs();
    std::scoped_lock lock(g_mutex);
    if (g_resolved && now >= g_nextList)
    {
        g_nextList = now + kListMs;
        for (uint16_t page = 0; page < g_pageCount; ++page)
        {
            sendList(page);
        }
    }
    std::erase_if(g_games, [&](const Game& g) { return now - g.seen >= kGameExpiryMs; });
    if (g_games.size() != g_loggedCount)
    {
        g_loggedCount = g_games.size();
        LOG("Matchmaker: {} game(s) listed", g_games.size());
    }
    if (now - g_lastAnnounced < kAnnounceMs)
    {
        return;
    }
    g_lastAnnounced = now;
    for (const Game& g : g_games)
    {
        Listed l{};
        std::memcpy(l.plain, g.plain, sizeof(l.plain));
        l.host = g.udp;
        out.push_back(l);
    }
}

bool Matchmaker::lookup(uint32_t address, uint16_t tcpPort, Target& out)
{
    if (!enabled())
    {
        return false;
    }
    std::scoped_lock lock(g_mutex);
    for (const Game& g : g_games)
    {
        if (g.udp.sin_addr.s_addr == address && get<uint16_t>(g.plain, 2) == tcpPort)
        {
            out = {g.id, g.udp, (g.flags & M::kFlagUdp) != 0};
            return true;
        }
    }
    return false;
}

void Matchmaker::join(uint32_t gameId)
{
    if (!enabled())
    {
        return;
    }
    const uint64_t now = nowMs();
    std::scoped_lock lock(g_mutex);
    if (g_pendingJoin == gameId && now - g_lastJoin < kJoinMinMs)
    {
        return;
    }
    g_pendingJoin = gameId;
    g_lastJoin = now;
    sendJoin(gameId);
}
