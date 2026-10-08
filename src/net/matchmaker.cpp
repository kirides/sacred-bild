#include "net/matchmaker.h"
#include "net/lan_protocol.h"
#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "config/net.h"
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
    // Stand-in IPv4 addresses for games reachable over IPv6 only: 198.18.0.0/15 (benchmarking, never routed).
    // Sacred has 4 bytes for a game's address (LAN announcement, list entry, TinCat's connect), and its LAN list
    // tells games apart by (address, name): each such game needs its own value.
    constexpr uint32_t kStandInBase = 0xC6120000;
    constexpr uint32_t kStandInMask = 0x1FFFF;
    constexpr const char* kFamily[] = {"IPv4", "IPv6"};

    bool g_host = false;

    // Echoed in the answers; not at DLL load (the OS's generator may load a DLL).
    uint32_t nonce()
    {
        static const uint32_t value = random32() | 1;
        return value;
    }

    // Host: identifies this gameserver's game over both address families; only the matchmaker learns it.
    uint64_t hostKey()
    {
        static const uint64_t value = random64();
        return value;
    }

    std::mutex g_mutex;

    // The matchmaker, per address family ([0] IPv4, [1] IPv6): it sees this PC under a different address in each,
    // so each has its own cookie.
    struct Server
    {
        Net::Address address;       // unset: the matchmaker has no address in this family
        uint8_t cookie[kCookieSize];
        bool hasCookie;
    };
    Server g_servers[2]{};
    std::string g_name;
    uint16_t g_port = kDefaultPort;
    bool g_resolved = false;
    uint64_t g_resolvedAt = 0;
    std::atomic<bool> g_resolving = false;

    // Host.
    uint8_t g_plain[LanAnnounce::kSize]{};
    bool g_hasGame = false;
    uint64_t g_nextRegister = 0;
    uint32_t g_refreshMs = kRegisterMs;
    uint32_t g_gameId = 0;
    Net::Address g_public[2]{};
    struct Punch
    {
        Net::Address to;
        uint32_t gameId;
        uint64_t at;
    };
    std::vector<Punch> g_punches;

    // Player.
    struct Game
    {
        uint32_t id;
        uint16_t flags;
        Net::Address ipv4, ipv6;
        uint8_t plain[LanAnnounce::kSize];
        uint64_t seen;
        uint32_t address;           // announced in the LAN list (network order)
    };
    std::vector<Game> g_games;
    uint16_t g_pageCount = 1;
    uint64_t g_nextList = 0;
    uint64_t g_lastAnnounced = 0;
    size_t g_loggedCount = SIZE_MAX;
    uint32_t g_pendingJoin = 0;
    uint64_t g_lastJoin = 0;

    int family(const Net::Address& a)
    {
        return Net::isV6(a) ? 1 : 0;
    }

    // Derived from the game id, so it stays the same while the game is listed; g_mutex held.
    uint32_t standInFor(uint32_t id)
    {
        uint32_t low = id & kStandInMask;
        const auto taken = [&](uint32_t a) { return std::ranges::any_of(g_games, [&](const Game& g) { return g.address == a; }); };
        for (;; low = (low + 1) & kStandInMask)
        {
            const uint32_t address = htonl(kStandInBase | low);
            if (low != 0 && low != kStandInMask && !taken(address))
            {
                return address;
            }
        }
    }

    // host, host:port, [IPv6]:port or a bare IPv6 address.
    void parseServer()
    {
        std::string text = Config::net.matchmaker;
        g_port = kDefaultPort;
        std::string portText;
        if (!text.empty() && text.front() == '[')
        {
            const size_t close = text.find(']');
            if (close != std::string::npos)
            {
                if (close + 1 < text.size() && text[close + 1] == ':')
                {
                    portText = text.substr(close + 2);
                }
                text = text.substr(1, close - 1);
            }
        }
        else if (const size_t colon = text.rfind(':'); colon != std::string::npos && text.find(':') == colon)
        {
            portText = text.substr(colon + 1);
            text.resize(colon);
        }
        const int port = portText.empty() ? 0 : std::atoi(portText.c_str());
        if (port > 0 && port < 0x10000)
        {
            g_port = static_cast<uint16_t>(port);
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
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            Net::Address found[2]{};
            if (getaddrinfo(g_name.c_str(), nullptr, &hints, &result) == 0)
            {
                for (const addrinfo* r = result; r; r = r->ai_next)
                {
                    Net::Address a = Net::fromSockaddr(r->ai_addr);
                    if (!Net::isSet(a) || Net::isSet(found[family(a)]))
                    {
                        continue;
                    }
                    if (Net::isV6(a))
                    {
                        a.Ipv6.sin6_port = htons(g_port);
                    }
                    else
                    {
                        a.Ipv4.sin_port = htons(g_port);
                    }
                    found[family(a)] = a;
                }
                freeaddrinfo(result);
            }
            {
                std::scoped_lock lock(g_mutex);
                const bool ok = Net::isSet(found[0]) || Net::isSet(found[1]);
                bool changed = false;
                for (int f = 0; ok && f < 2; ++f)
                {
                    if (!Net::same(found[f], g_servers[f].address))
                    {
                        g_servers[f] = {found[f], {}, false};
                        changed = true;
                    }
                }
                if (ok)
                {
                    g_resolved = true;
                }
                if (changed)
                {
                    LOG("Matchmaker: {} = {}{}{}", g_name, Net::isSet(found[0]) ? Net::toString(found[0]) : "",
                        Net::isSet(found[0]) && Net::isSet(found[1]) ? ", " : "",
                        Net::isSet(found[1]) ? Net::toString(found[1]) : "");
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

    // A request with the nonce and the cookie, to the matchmaker's address in each family (or only in `only`);
    // g_mutex held.
    void request(uint8_t* p, size_t size, char type, int only = -1)
    {
        magic(p, kMatchmaker, type);
        put<uint32_t>(p, 4, kVersion);
        put<uint32_t>(p, 8, nonce());
        for (int f = 0; f < 2; ++f)
        {
            const Server& server = g_servers[f];
            if ((only >= 0 && f != only) || !Net::isSet(server.address))
            {
                continue;
            }
            if (server.hasCookie)
            {
                std::memcpy(p + 16, server.cookie, kCookieSize);
            }
            else
            {
                std::memset(p + 16, 0, kCookieSize);
            }
            UdpEndpoint::sendTo(p, static_cast<int>(size), server.address);
        }
    }

    void sendRegister(int only = -1)
    {
        uint8_t p[M::kRegisterSize] = {};
        put<uint16_t>(p, 12, Config::net.udp ? M::kFlagUdp : 0);
        put<uint64_t>(p, 32, hostKey());
        std::memcpy(p + 40, g_plain, LanAnnounce::kSize);
        request(p, sizeof(p), M::Register, only);
    }

    void sendList(uint16_t page, int only = -1)
    {
        uint8_t p[M::kListSize] = {};
        put<uint16_t>(p, 12, page);
        request(p, sizeof(p), M::List, only);
    }

    void sendJoin(uint32_t gameId, int only = -1)
    {
        uint8_t p[M::kJoinSize] = {};
        put<uint32_t>(p, 12, gameId);
        request(p, sizeof(p), M::Join, only);
    }

    // Player: a game from GAMES or JOINED; g_mutex held.
    void update(uint32_t id, uint16_t flags, const Net::Address& ipv4, const Net::Address& ipv6, const uint8_t* plain,
        uint64_t now)
    {
        auto it = std::ranges::find(g_games, id, &Game::id);
        if (!Net::isSet(ipv4) && (!Net::isSet(ipv6) || !(flags & M::kFlagUdp)))
        {
            // Without IPv4 only the UDP transport reaches it.
            if (it != g_games.end())
            {
                g_games.erase(it);
            }
            return;
        }
        if (it == g_games.end())
        {
            if (!plain)
            {
                return;
            }
            g_games.push_back({id});
            it = g_games.end() - 1;
        }
        it->flags = flags;
        it->ipv4 = ipv4;
        it->ipv6 = ipv6;
        if (plain)
        {
            std::memcpy(it->plain, plain, LanAnnounce::kSize);
            it->seen = now;
        }
        if (Net::isSet(ipv4))
        {
            it->address = ipv4.Ipv4.sin_addr.s_addr;
        }
        else if (!Matchmaker::isStandIn(it->address))
        {
            it->address = standInFor(id);
            LOG("Matchmaker: game '{}' is reachable over IPv6 only ({}); listed as {}", LanAnnounce::gameName(it->plain),
                Net::toString(ipv6), Net::toString(Net::ipv4(it->address, get<uint16_t>(it->plain, 2))));
        }
    }

    void onPacket(const uint8_t* p, int n, const Net::Address& from)
    {
        if (n < 12 || get<uint32_t>(p, 4) != kVersion)
        {
            return;
        }
        std::scoped_lock lock(g_mutex);
        const int f = family(from);
        if (!Net::same(from, g_servers[f].address))
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
                const Net::Address player = Net::decode(p + 12);
                if (Net::isSet(player))
                {
                    LOG("Matchmaker: a player at {} joins, opening the way", Net::toString(player));
                    for (const uint64_t delay : kPunchDelaysMs)
                    {
                        g_punches.push_back({player, g_gameId, now + delay});
                    }
                    UdpEndpoint::wake();
                }
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
                std::memcpy(g_servers[f].cookie, p + 16, kCookieSize);
                g_servers[f].hasCookie = true;
                if (g_host && g_hasGame)
                {
                    sendRegister(f);
                }
                if (!g_host)
                {
                    g_nextList = 0;
                    if (g_pendingJoin)
                    {
                        sendJoin(g_pendingJoin, f);
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
                const Net::Address seen = Net::decode(p + 20);
                if (id != g_gameId || !Net::same(seen, g_public[f]))
                {
                    LOG("Matchmaker: game '{}' listed (id {:08x}); players reach this gameserver at {} over {}",
                        LanAnnounce::gameName(g_plain), id, Net::toString(seen), kFamily[f]);
                }
                g_gameId = id;
                g_public[f] = seen;
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
                    update(get<uint32_t>(e, 0), get<uint16_t>(e, 4), Net::decode(e + 8), Net::decode(e + 26), e + 44,
                        now);
                }
            }
            break;
        case M::Joined:
            if (!g_host && n >= static_cast<int>(M::kJoinedSize))
            {
                const uint32_t id = get<uint32_t>(p, 12);
                const Net::Address ipv4 = Net::decode(p + 20), ipv6 = Net::decode(p + 38);
                if (!Net::isSet(ipv4) && !Net::isSet(ipv6))
                {
                    LOG("Matchmaker: game {:08x} is gone", id);
                }
                else
                {
                    update(id, get<uint16_t>(p, 16), ipv4, ipv6, nullptr, now);
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
    return !Config::net.matchmaker.empty();
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
    if (host && !Config::net.publish)
    {
        LOG("Matchmaker: hosted games are not published ([Net] Publish=0)");
    }
}

void Matchmaker::publish(const uint8_t* plain)
{
    if (!enabled() || !g_host || !Config::net.publish)
    {
        return;
    }
    std::scoped_lock lock(g_mutex);
    const bool changed = !g_hasGame || std::memcmp(g_plain + 8, plain + 8, LanAnnounce::kSize - 8) != 0;
    std::memcpy(g_plain, plain, LanAnnounce::kSize);
    if (!g_hasGame)
    {
        LOG("Matchmaker: publishing '{}' at {} port {}", LanAnnounce::gameName(plain), g_name, g_port);
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
        l.address = g.address;
        // Games on the UDP transport that IPv6 reaches say so in their name; all others stay as they are.
        if ((g.flags & M::kFlagUdp) && Net::isSet(g.ipv6))
        {
            LanAnnounce::prefixName(l.plain, Net::isSet(g.ipv4) ? L"[IPv4+6] " : L"[IPv6] ");
        }
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
        if (g.address == address && get<uint16_t>(g.plain, 2) == tcpPort)
        {
            out = {g.id, g.ipv4, g.ipv6, (g.flags & M::kFlagUdp) != 0};
            return true;
        }
    }
    return false;
}

bool Matchmaker::isStandIn(uint32_t address)
{
    return (ntohl(address) & ~kStandInMask) == kStandInBase;
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
