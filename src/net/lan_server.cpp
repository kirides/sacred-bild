#include "net/lan_server.h"
#include "net/adapters.h"
#include "net/lan_protocol.h"
#include "net/matchmaker.h"
#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "game/gameserver_addr.h"
#include "config/net.h"
#include "log.h"
#include "patch.h"

#include <winsock2.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using namespace GameServer;
    using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);

    SendFn g_send = nullptr;

    struct Subscriber
    {
        sockaddr_in addr;
        uint64_t until;
    };

    constexpr size_t kMaxSubscribers = 32;
    constexpr uint64_t kAdapterRefreshMs = 10000;
    constexpr uint32_t kLoggedAnnouncements = 3;

    std::mutex g_mutex;     // announcements come from the main loop and from player join/leave handling
    std::vector<Subscriber> g_subscribers;
    std::vector<Net::LocalAddress> g_addresses;
    uint32_t g_limitedIf = 0;
    uint64_t g_addressesTime = 0;
    uint32_t g_announcements = 0;
    bool g_loggedEndpoint = false;

    const uint8_t* networkObject()
    {
        const auto* app = *reinterpret_cast<const uint8_t* const*>(Addr::g_pApp);
        return app ? *reinterpret_cast<const uint8_t* const*>(app + Addr::app_net) : nullptr;
    }

    std::string endpoint(const sockaddr_in& a)
    {
        return Fmt::format("{}:{}", Net::toString(a.sin_addr.s_addr), ntohs(a.sin_port));
    }

    void refreshAddresses(uint64_t now)
    {
        g_addressesTime = now;
        auto addresses = Net::localAddresses();
        const uint32_t limitedIf = Net::limitedBroadcastInterface();
        if (addresses == g_addresses && limitedIf == g_limitedIf)
        {
            return;
        }
        g_addresses = std::move(addresses);
        g_limitedIf = limitedIf;
        std::string list;
        for (const auto& a : g_addresses)
        {
            list += Fmt::format("{}{} (adapter {}, {})", list.empty() ? "" : ", ", Net::toString(a.address), a.ifIndex,
                a.ifIndex == g_limitedIf ? "game's broadcast"
                    : a.broadcast ? "broadcast " + Net::toString(a.broadcast) : "no broadcast");
        }
        LOG("LAN relay: addresses {}", list.empty() ? "none" : list);
    }

    // On the endpoint's I/O thread. Players subscribe over IPv4 only (Sacred's LAN list).
    void onSubscription(const uint8_t* msg, int n, const Net::Address& address)
    {
        if (Net::isV6(address) || !LanRelay::hasHeader(msg, n, LanRelay::kSubscribeMagic, LanRelay::kSubscribeSize))
        {
            return;
        }
        const sockaddr_in& from = address.Ipv4;
        std::scoped_lock lock(g_mutex);
        const uint64_t now = GetTickCount64();
        const auto it = std::ranges::find_if(g_subscribers, [&](const Subscriber& s) {
            return s.addr.sin_addr.s_addr == from.sin_addr.s_addr && s.addr.sin_port == from.sin_port;
        });
        if (it != g_subscribers.end())
        {
            it->until = now + LanRelay::kSubscriptionMs;
        }
        else if (g_subscribers.size() < kMaxSubscribers)
        {
            g_subscribers.push_back({from, now + LanRelay::kSubscriptionMs});
            LOG("LAN relay: {} subscribed", endpoint(from));
        }
    }

    void dropExpiredSubscribers(uint64_t now)
    {
        std::erase_if(g_subscribers, [&](const Subscriber& s) {
            if (s.until > now)
            {
                return false;
            }
            LOG("LAN relay: {} unsubscribed (no renewal)", endpoint(s.addr));
            return true;
        });
    }

    int announce(SOCKET s, const char* buf, int len, int flags, const uint8_t* plain)
    {
        std::scoped_lock lock(g_mutex);
        sockaddr_in target{};
        int targetLen = sizeof(target);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&target), &targetLen) == SOCKET_ERROR)
        {
            return g_send(s, buf, len, flags);
        }
        // The endpoint (relay subscriptions, UDP transport, matchmaker) on [Net] Port; the first announcement
        // comes after TinCat listens, which usually opened it already.
        const bool endpoint = UdpEndpoint::open(static_cast<uint16_t>(Config::net.port), true);
        if (!g_loggedEndpoint)
        {
            g_loggedEndpoint = true;
            if (!endpoint)
            {
                LOG("LAN relay: UDP port {} is not available; no announcements on other adapters, players who list "
                    "this PC under [Net] Hosts won't see its games", Config::net.port);
            }
        }
        Matchmaker::publish(plain);
        if (!Config::net.relay)
        {
            return g_send(s, buf, len, flags);
        }
        const uint64_t now = GetTickCount64();
        if (g_addressesTime == 0 || now - g_addressesTime >= kAdapterRefreshMs)
        {
            refreshAddresses(now);
        }

        // The game's own broadcast reaches the adapter with the best route; it gets that adapter's address.
        uint8_t wire[LanAnnounce::kWireSize];
        int result;
        const auto limited = std::ranges::find(g_addresses, g_limitedIf, &Net::LocalAddress::ifIndex);
        if (limited != g_addresses.end())
        {
            LanAnnounce::encode(plain, limited->address, wire);
            result = g_send(s, reinterpret_cast<const char*>(wire), sizeof(wire), flags);
        }
        else
        {
            result = g_send(s, buf, len, flags);
        }

        // Every other adapter (VPN adapters among them) gets a subnet broadcast with its own address, and the
        // subscribers the plain announcement (they put in the address they received it from).
        int broadcasts = 0;
        if (endpoint)
        {
            for (const auto& a : g_addresses)
            {
                if (a.ifIndex == g_limitedIf || !a.broadcast)
                {
                    continue;
                }
                LanAnnounce::encode(plain, a.address, wire);
                UdpEndpoint::sendTo(wire, sizeof(wire), Net::ipv4(a.broadcast, ntohs(target.sin_port)));
                ++broadcasts;
            }
            dropExpiredSubscribers(now);
            uint8_t msg[LanRelay::kAnnounceSize];
            LanRelay::writeHeader(msg, LanRelay::kAnnounceMagic);
            std::memcpy(msg + 8, plain, LanAnnounce::kSize);
            for (const auto& sub : g_subscribers)
            {
                UdpEndpoint::sendTo(msg, sizeof(msg), Net::ipv4(sub.addr.sin_addr.s_addr, ntohs(sub.addr.sin_port)));
            }
        }

        if (g_announcements++ < kLoggedAnnouncements)
        {
            uint16_t port = 0;
            std::memcpy(&port, plain + 2, 2);
            LOG("LAN announcement '{}' (gameserver port {}) to UDP port {}: game's broadcast {}, {} subnet broadcasts, "
                "{} subscribers", LanAnnounce::gameName(plain), port, ntohs(target.sin_port),
                limited != g_addresses.end() ? "as " + Net::toString(limited->address) : std::string("unchanged"),
                broadcasts, g_subscribers.size());
        }
        return result;
    }

    int WSAAPI hookSend(SOCKET s, const char* buf, int len, int flags)
    {
        const uint8_t* net = networkObject();
        if (net && s == *reinterpret_cast<const SOCKET*>(net + Addr::net_pingSocket))
        {
            return announce(s, buf, len, flags, net + Addr::net_announcement);
        }
        return g_send(s, buf, len, flags);
    }
}

void LanServer::install()
{
    if (Config::net.relay)
    {
        UdpEndpoint::onPacket(UdpProto::kLanRelay, onSubscription);
    }
    else
    {
        LOG("LAN relay off ([Net] Relay=0)");
        if (!Matchmaker::enabled())
        {
            return;
        }
    }
    g_send = reinterpret_cast<SendFn>(Patch::iat("WS2_32.dll", "send", reinterpret_cast<void*>(&hookSend)));
    if (g_send && Config::net.relay)
    {
        LOG("LAN relay: announcements go out on every adapter and to subscribers");
    }
}
