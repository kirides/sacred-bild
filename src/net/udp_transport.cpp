#include "net/udp_transport.h"
#include "net/adapters.h"
#include "net/lan_client.h"
#include "net/matchmaker.h"
#include "net/udp_endpoint.h"
#include "net/udp_protocol.h"
#include "config.h"
#include "log.h"

#include <winsock2.h>
#include <ikcp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace UdpProto;

namespace
{
    constexpr int kKcpMtu = static_cast<int>(kMaxDatagram - T::kHeaderSize);
    constexpr int kSendWindow = 128;        // segments in flight (~150 KB)
    constexpr int kRecvWindow = 256;
    constexpr int kMaxQueued = 2 * kSendWindow;     // send() blocks beyond this, like a full TCP send buffer
    constexpr uint32_t kHelloIntervalMs = 200;
    constexpr uint32_t kHandshakeMs = 1500;
    constexpr uint32_t kHandshakeMatchmakerMs = 3000;   // the host punches towards us first
    constexpr uint32_t kJoinIntervalMs = 1000;
    constexpr uint32_t kKeepAliveMs = 1000;
    constexpr uint32_t kProbeMs = 200;          // keep-alive while the other side is silent: notices its return
    constexpr uint32_t kDeadMs = 45000;
    constexpr uint32_t kSilenceResendMs = 300;
    constexpr uint32_t kRejoinMs = 2000;
    constexpr uint32_t kLingerMs = 5000;
    constexpr uint32_t kAcceptMs = 30000;
    constexpr uint32_t kStatsMs = 5 * 60000;
    constexpr size_t kMaxSessions = 64;
    constexpr uint32_t kTinCatMagic = 0xDABAFBEF;
    constexpr int kTinCatHeader = 0x1C;

    std::string endpoint(const sockaddr_in& a)
    {
        return std::format("{}:{}", Net::toString(a.sin_addr.s_addr), ntohs(a.sin_port));
    }

    bool sameAddress(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
}

struct UdpTransport::Session
{
    std::mutex m;
    std::condition_variable cv;
    bool host = false;
    Status status = Status::Connecting;
    ikcpcb* kcp = nullptr;
    uint32_t id = 0;
    uint64_t token = 0;
    sockaddr_in peer{};

    std::vector<uint8_t> rx;
    size_t rxPos = 0;
    bool peerClosed = false;    // CLOSE received: recv() returns 0 once the data is read
    bool dead = false;          // nothing heard for kDeadMs
    bool shut = false;          // shutdown()
    bool localClosed = false;   // closesocket(): linger, then remove
    bool accepted = false;      // host: handed to TinCat

    uint64_t created = 0, lastRecv = 0, lastSend = 0, closedAt = 0, lastStats = 0;

    // Player handshake.
    sockaddr_in tcpTarget{};
    uint32_t nonce = 0;
    uint8_t cookie[kCookieSize]{};
    bool hasCookie = false;
    uint64_t nextHello = 0, deadline = 0, nextJoin = 0;
    uint32_t gameId = 0;
    const char* failure = nullptr;

    uint64_t bytesIn = 0, bytesOut = 0;
    uint32_t datagramsIn = 0, datagramsOut = 0, moves = 0;
};

namespace
{
    using UdpTransport::Session;
    using UdpTransport::SessionPtr;
    using UdpTransport::Status;

    bool g_host = false;
    bool g_hostAccepts = false;

    std::mutex g_mutex;
    std::vector<SessionPtr> g_sessions;
    std::deque<SessionPtr> g_accepts;
    uint16_t g_listenerPort = 0;

    std::mutex g_changeMutex;
    std::condition_variable g_changeCv;
    std::atomic<uint64_t> g_changes = 0;

    void notifyChange()
    {
        {
            std::scoped_lock lock(g_changeMutex);
            ++g_changes;
        }
        g_changeCv.notify_all();
    }

    uint32_t kcpNow(uint64_t now)
    {
        return static_cast<uint32_t>(now);
    }

    std::string name(const Session& s)
    {
        return std::format("{} {} (session {:08x})", s.host ? "player" : "gameserver", endpoint(s.peer), s.id);
    }

    void sendControl(Session& s, char type)
    {
        uint8_t p[T::kHeaderSize];
        magic(p, kTransport, type);
        put<uint32_t>(p, 4, s.id);
        put<uint64_t>(p, 8, s.token);
        UdpEndpoint::sendTo(p, sizeof(p), s.peer);
        s.lastSend = nowMs();
        ++s.datagramsOut;
    }

    int kcpOutput(const char* buf, int len, ikcpcb*, void* user)
    {
        auto& s = *static_cast<Session*>(user);
        uint8_t p[kMaxDatagram];
        magic(p, kTransport, T::Data);
        put<uint32_t>(p, 4, s.id);
        put<uint64_t>(p, 8, s.token);
        std::memcpy(p + T::kHeaderSize, buf, len);
        UdpEndpoint::sendTo(p, static_cast<int>(T::kHeaderSize) + len, s.peer);
        s.lastSend = nowMs();
        ++s.datagramsOut;
        return 0;
    }

    void createKcp(Session& s, uint64_t now)
    {
        s.kcp = ikcp_create(s.id, &s);
        ikcp_setoutput(s.kcp, kcpOutput);
        ikcp_setmtu(s.kcp, kKcpMtu);
        ikcp_wndsize(s.kcp, kSendWindow, kRecvWindow);
        // No delay: minimum RTO 30 ms, back-off 1.5x; a 10 ms clock; resend once a later segment was acked;
        // no congestion window (the game's traffic is small, and a window that collapses after a loss is what
        // makes TCP stall).
        ikcp_nodelay(s.kcp, 1, 10, 1, 1);
        s.kcp->stream = 1;
        s.kcp->dead_link = 0xFFFFFFFF;  // our own time-out decides
        ikcp_update(s.kcp, kcpNow(now));
    }

    // After an outage KCP's resend timers have backed off to seconds; once the peer is heard again, resend now.
    void resendNow(Session& s)
    {
        for (IQUEUEHEAD* p = s.kcp->snd_buf.next; p != &s.kcp->snd_buf; p = p->next)
        {
            auto* seg = iqueue_entry(p, IKCPSEG, node);
            if (seg->xmit > 0)
            {
                seg->rto = static_cast<IUINT32>(s.kcp->rx_rto);
                seg->resendts = s.kcp->current;
            }
        }
    }

    void flush(Session& s, uint64_t now)
    {
        s.kcp->current = kcpNow(now);
        ikcp_flush(s.kcp);
    }

    void drain(Session& s)
    {
        for (;;)
        {
            const int size = ikcp_peeksize(s.kcp);
            if (size <= 0)
            {
                return;
            }
            const size_t old = s.rx.size();
            s.rx.resize(old + size);
            const int n = ikcp_recv(s.kcp, reinterpret_cast<char*>(s.rx.data() + old), size);
            s.rx.resize(old + std::max(n, 0));
            if (n <= 0)
            {
                return;
            }
            s.bytesIn += n;
        }
    }

    void logStats(const Session& s, uint64_t now, const char* what)
    {
        const uint64_t minutes = (now - s.created) / 60000;
        LOG("UDP {}: {} {} min, RTT {} ms (+-{}), received {} KB, sent {} KB, {} datagrams in / {} out, {} resent "
            "after a time-out, {} address changes", what, name(s), minutes, s.kcp ? s.kcp->rx_srtt : 0,
            s.kcp ? s.kcp->rx_rttval : 0, s.bytesIn / 1024, s.bytesOut / 1024, s.datagramsIn, s.datagramsOut,
            s.kcp ? s.kcp->xmit : 0, s.moves);
    }

    SessionPtr findById(uint32_t id)
    {
        std::scoped_lock lock(g_mutex);
        for (const auto& s : g_sessions)
        {
            if (s->id == id)
            {
                return s;
            }
        }
        return nullptr;
    }

    void sendHello(Session& s)
    {
        uint8_t p[T::kHelloSize] = {};
        magic(p, kTransport, T::Hello);
        put<uint32_t>(p, 4, kVersion);
        put<uint32_t>(p, 8, s.nonce);
        put<uint16_t>(p, 12, ntohs(s.tcpTarget.sin_port));
        if (s.hasCookie)
        {
            std::memcpy(p + 16, s.cookie, kCookieSize);
        }
        UdpEndpoint::sendTo(p, sizeof(p), s.peer);
        ++s.datagramsOut;
    }

    void refuse(const sockaddr_in& to, uint32_t nonce, uint32_t reason)
    {
        uint8_t p[T::kRefusedSize] = {};
        magic(p, kTransport, T::Refused);
        put<uint32_t>(p, 4, kVersion);
        put<uint32_t>(p, 8, nonce);
        put<uint32_t>(p, 12, reason);
        UdpEndpoint::sendTo(p, sizeof(p), to);
    }

    void sendWelcome(const Session& s, const sockaddr_in& to, uint32_t nonce)
    {
        uint8_t p[T::kWelcomeSize] = {};
        magic(p, kTransport, T::Welcome);
        put<uint32_t>(p, 4, kVersion);
        put<uint32_t>(p, 8, nonce);
        put<uint32_t>(p, 12, s.id);
        put<uint64_t>(p, 16, s.token);
        UdpEndpoint::sendTo(p, sizeof(p), to);
    }

    // --- host ---

    void onHello(const uint8_t* p, int n, const sockaddr_in& from)
    {
        if (n < static_cast<int>(T::kHelloSize))
        {
            return;     // shorter than the answers: could be used for amplification
        }
        const uint32_t nonce = get<uint32_t>(p, 8);
        if (get<uint32_t>(p, 4) != kVersion)
        {
            refuse(from, nonce, T::BadVersion);
            return;
        }
        if (!g_hostAccepts)
        {
            refuse(from, nonce, T::Disabled);
            return;
        }
        const uint16_t tcpPort = get<uint16_t>(p, 12);
        uint16_t listener;
        {
            std::scoped_lock lock(g_mutex);
            listener = g_listenerPort;
        }
        if (listener == 0 || tcpPort != listener)
        {
            refuse(from, nonce, T::NotListening);
            return;
        }
        if (!validCookie(from.sin_addr.s_addr, from.sin_port, nonce, p + 16))
        {
            uint8_t c[T::kChallengeSize] = {};
            magic(c, kTransport, T::Challenge);
            put<uint32_t>(c, 4, kVersion);
            put<uint32_t>(c, 8, nonce);
            cookie(from.sin_addr.s_addr, from.sin_port, nonce, c + 16);
            UdpEndpoint::sendTo(c, T::kChallengeSize, from);
            return;
        }

        std::unique_lock lock(g_mutex);
        // A repeated HELLO (our WELCOME got lost): the same answer.
        for (const auto& s : g_sessions)
        {
            if (s->host && s->nonce == nonce && sameAddress(s->peer, from))
            {
                sendWelcome(*s, from, nonce);
                return;
            }
        }
        if (g_sessions.size() >= kMaxSessions)
        {
            lock.unlock();
            refuse(from, nonce, T::Full);
            return;
        }
        auto s = std::make_shared<Session>();
        s->host = true;
        s->nonce = nonce;
        s->peer = from;
        do
        {
            s->id = random32();
        } while (s->id == 0 || std::ranges::any_of(g_sessions, [&](const SessionPtr& o) { return o->id == s->id; }));
        s->token = random64();
        const uint64_t now = nowMs();
        s->created = s->lastRecv = s->lastSend = s->lastStats = now;
        createKcp(*s, now);
        s->status = Status::Open;
        g_sessions.push_back(s);
        g_accepts.push_back(s);
        lock.unlock();
        sendWelcome(*s, from, nonce);
        LOG("UDP: {} connected", name(*s));
        notifyChange();
    }

    // --- player ---

    SessionPtr findConnecting(uint32_t nonce)
    {
        std::scoped_lock lock(g_mutex);
        for (const auto& s : g_sessions)
        {
            if (!s->host && s->nonce == nonce)
            {
                return s;
            }
        }
        return nullptr;
    }

    void fail(Session& s, const char* why)
    {
        s.status = Status::Failed;
        s.failure = why;
        LOG("UDP: no connection to the gameserver at {} ({}): connecting over TCP", endpoint(s.peer), why);
        s.cv.notify_all();
        notifyChange();
    }

    void onHandshakeReply(char type, const uint8_t* p, int n, const sockaddr_in& from)
    {
        if (n < 12 || get<uint32_t>(p, 4) != kVersion)
        {
            return;
        }
        const SessionPtr s = findConnecting(get<uint32_t>(p, 8));
        if (!s)
        {
            return;
        }
        std::scoped_lock lock(s->m);
        if (s->status != Status::Connecting || !sameAddress(s->peer, from))
        {
            return;
        }
        const uint64_t now = nowMs();
        ++s->datagramsIn;
        if (type == T::Challenge && n >= static_cast<int>(T::kChallengeSize))
        {
            std::memcpy(s->cookie, p + 16, kCookieSize);
            s->hasCookie = true;
            sendHello(*s);
            s->nextHello = now + kHelloIntervalMs;
        }
        else if (type == T::Welcome && n >= static_cast<int>(T::kWelcomeSize))
        {
            s->id = get<uint32_t>(p, 12);
            s->token = get<uint64_t>(p, 16);
            s->lastRecv = s->lastSend = s->lastStats = now;
            createKcp(*s, now);
            s->status = Status::Open;
            LOG("UDP: connected to {} in {} ms{}", name(*s), now - s->created,
                s->gameId ? " (through the matchmaker)" : "");
            s->cv.notify_all();
            notifyChange();
        }
        else if (type == T::Refused && n >= static_cast<int>(T::kRefusedSize))
        {
            const uint32_t reason = get<uint32_t>(p, 12);
            fail(*s, reason == T::Disabled ? "the host has [Net] Udp off"
                : reason == T::NotListening ? "no game on that port"
                : reason == T::Full ? "the host is full"
                : "other SacredBild version");
        }
    }

    // --- both ---

    void onSessionPacket(char type, const uint8_t* p, int n, const sockaddr_in& from)
    {
        if (n < static_cast<int>(T::kHeaderSize))
        {
            return;
        }
        const SessionPtr s = findById(get<uint32_t>(p, 4));
        if (!s)
        {
            return;
        }
        std::scoped_lock lock(s->m);
        if (s->token != get<uint64_t>(p, 8) || !s->kcp)
        {
            return;
        }
        const uint64_t now = nowMs();
        if (!sameAddress(s->peer, from))
        {
            // Roaming or NAT rebinding: answer where the other side is now.
            ++s->moves;
            LOG("UDP: {} now at {}", name(*s), endpoint(from));
            s->peer = from;
        }
        const uint64_t silence = now - s->lastRecv;
        s->lastRecv = now;
        ++s->datagramsIn;
        if (s->dead)
        {
            s->dead = false;
            LOG("UDP: {} heard again", name(*s));
        }
        s->kcp->current = kcpNow(now);     // ikcp_input measures the RTT against it
        if (type == T::Data)
        {
            ikcp_input(s->kcp, reinterpret_cast<const char*>(p + T::kHeaderSize), n - static_cast<long>(T::kHeaderSize));
            drain(*s);
        }
        else if (type == T::Close)
        {
            if (!s->peerClosed)
            {
                LOG("UDP: {} closed the connection", name(*s));
            }
            s->peerClosed = true;
        }
        if (silence >= kSilenceResendMs)
        {
            resendNow(*s);
        }
        // ACKs right away, not with the next 10 ms tick: the sender's RTT and resend timing depend on them.
        flush(*s, now);
        s->cv.notify_all();
        notifyChange();
    }

    void onPacket(const uint8_t* p, int n, const sockaddr_in& from)
    {
        switch (static_cast<char>(p[3]))
        {
        case T::Hello:
            if (g_host)
            {
                onHello(p, n, from);
            }
            break;
        case T::Challenge:
        case T::Welcome:
        case T::Refused:
            if (!g_host)
            {
                onHandshakeReply(static_cast<char>(p[3]), p, n, from);
            }
            break;
        case T::Data:
        case T::KeepAlive:
        case T::Close:
            onSessionPacket(static_cast<char>(p[3]), p, n, from);
            break;
        default:
            break;      // PUNCH: only opens NAT mappings
        }
    }

    // Player: the endpoint of the gameserver at `tcpTarget`: from the matchmaker, else the relay port.
    void target(Session& s)
    {
        Matchmaker::Target t{};
        if (Matchmaker::lookup(s.tcpTarget.sin_addr.s_addr, ntohs(s.tcpTarget.sin_port), t))
        {
            s.gameId = t.gameId;
            s.peer = t.udp;
            return;
        }
        s.peer = s.tcpTarget;
        s.peer.sin_port = htons(LanClient::relayPort(s.tcpTarget.sin_addr.s_addr));
    }

    uint32_t tick(uint64_t now)
    {
        std::vector<SessionPtr> sessions;
        {
            std::scoped_lock lock(g_mutex);
            sessions = g_sessions;
        }
        std::vector<SessionPtr> remove;
        std::vector<uint32_t> joins;
        for (const auto& s : sessions)
        {
            std::scoped_lock lock(s->m);
            if (s->status == Status::Connecting)
            {
                if (now >= s->deadline)
                {
                    fail(*s, "no answer");
                }
                else
                {
                    if (now >= s->nextHello)
                    {
                        target(*s);     // the matchmaker may have a newer address for the host
                        sendHello(*s);
                        s->nextHello = now + kHelloIntervalMs;
                    }
                    if (s->gameId && now >= s->nextJoin)
                    {
                        joins.push_back(s->gameId);
                        s->nextJoin = now + kJoinIntervalMs;
                    }
                }
            }
            else if (s->status == Status::Open)
            {
                ikcp_update(s->kcp, kcpNow(now));
                if (!s->dead && now - s->lastRecv >= kDeadMs)
                {
                    s->dead = true;
                    LOG("UDP: nothing from {} for {} s: connection lost", name(*s), kDeadMs / 1000);
                    s->cv.notify_all();
                    notifyChange();
                }
                const uint32_t keepAlive = now - s->lastRecv >= kSilenceResendMs ? kProbeMs : kKeepAliveMs;
                if (!s->dead && !s->peerClosed && now - s->lastSend >= keepAlive)
                {
                    sendControl(*s, T::KeepAlive);
                }
                // A player that moved may be behind a NAT the host hasn't punched yet: get introduced again.
                if (!s->host && s->gameId && now - s->lastRecv >= kRejoinMs && now >= s->nextJoin)
                {
                    joins.push_back(s->gameId);
                    s->nextJoin = now + kRejoinMs;
                }
                if (now - s->lastStats >= kStatsMs)
                {
                    s->lastStats = now;
                    logStats(*s, now, "connection");
                }
            }

            const bool unaccepted = s->host && !s->accepted && !s->localClosed && now - s->created >= kAcceptMs;
            if (unaccepted)
            {
                s->localClosed = true;
                s->closedAt = now;
            }
            if (s->localClosed)
            {
                const bool sent = !s->kcp || ikcp_waitsnd(s->kcp) == 0;
                if (sent || s->dead || s->peerClosed || now - s->closedAt >= kLingerMs)
                {
                    if (s->kcp)
                    {
                        if (!s->dead && !s->peerClosed)
                        {
                            sendControl(*s, T::Close);
                            sendControl(*s, T::Close);
                        }
                        logStats(*s, now, "closed");
                        ikcp_release(s->kcp);
                        s->kcp = nullptr;
                    }
                    remove.push_back(s);
                }
            }
        }
        if (!remove.empty())
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& s : remove)
            {
                std::erase(g_sessions, s);
                std::erase(g_accepts, s);
            }
        }
        for (const uint32_t id : joins)
        {
            Matchmaker::join(id);
        }
        return sessions.empty() ? 1000 : 10;
    }

    void wsaError(int error)
    {
        WSASetLastError(error);
    }
}

void UdpTransport::install(bool host)
{
    g_host = host;
    g_hostAccepts = host && g_config.netUdp;
    UdpEndpoint::onPacket(kTransport, onPacket);
    UdpEndpoint::onTick(tick);
}

UdpTransport::SessionPtr UdpTransport::connect(const sockaddr_in& tcpTarget)
{
    if (g_host || !UdpEndpoint::open(0, false))
    {
        return nullptr;
    }
    auto s = std::make_shared<Session>();
    s->tcpTarget = tcpTarget;
    do
    {
        s->nonce = random32();
    } while (s->nonce == 0);
    target(*s);
    const uint64_t now = nowMs();
    s->created = now;
    s->deadline = now + (s->gameId ? kHandshakeMatchmakerMs : kHandshakeMs);
    {
        std::scoped_lock lock(g_mutex);
        g_sessions.push_back(s);
    }
    LOG("UDP: connecting to the gameserver at {} (game port {}){}", endpoint(s->peer), ntohs(tcpTarget.sin_port),
        s->gameId ? std::format(", matchmaker game {:08x}", s->gameId) : std::string());
    UdpEndpoint::wake();
    return s;
}

UdpTransport::Status UdpTransport::status(Session& s)
{
    std::scoped_lock lock(s.m);
    return s.status;
}

bool UdpTransport::waitConnected(Session& s)
{
    std::unique_lock lock(s.m);
    s.cv.wait(lock, [&] { return s.status != Status::Connecting; });
    return s.status == Status::Open;
}

void UdpTransport::setListener(uint16_t tcpPort)
{
    std::scoped_lock lock(g_mutex);
    if (g_listenerPort != tcpPort && g_hostAccepts)
    {
        if (tcpPort)
        {
            LOG("UDP: accepting players for the game on TCP port {}", tcpPort);
        }
        else
        {
            LOG("UDP: not accepting players");
        }
    }
    g_listenerPort = tcpPort;
}

bool UdpTransport::hasPendingAccept()
{
    std::scoped_lock lock(g_mutex);
    return !g_accepts.empty();
}

UdpTransport::SessionPtr UdpTransport::accept()
{
    SessionPtr s;
    {
        std::scoped_lock lock(g_mutex);
        if (g_accepts.empty())
        {
            return nullptr;
        }
        s = g_accepts.front();
        g_accepts.pop_front();
    }
    std::scoped_lock lock(s->m);
    s->accepted = true;
    return s;
}

int UdpTransport::send(Session& s, const char* data, int size, bool nonBlocking)
{
    std::unique_lock lock(s.m);
    for (;;)
    {
        if (s.status != Status::Open)
        {
            wsaError(WSAENOTCONN);
            return SOCKET_ERROR;
        }
        if (s.dead || s.peerClosed || s.shut || s.localClosed)
        {
            wsaError(WSAECONNRESET);
            return SOCKET_ERROR;
        }
        if (ikcp_waitsnd(s.kcp) < kMaxQueued)
        {
            break;
        }
        if (nonBlocking)
        {
            wsaError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        s.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    // ikcp_send refuses more than its receive window in segments at once.
    const int chunk = 64 * static_cast<int>(s.kcp->mss);
    int sent = 0;
    while (sent < size)
    {
        const int n = ikcp_send(s.kcp, data + sent, std::min(size - sent, chunk));
        if (n <= 0)
        {
            break;
        }
        sent += n;
    }
    if (sent == 0 && size > 0)
    {
        wsaError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    s.bytesOut += sent;
    // TinCat sends each message's 28-byte header and its payload with two send() calls: the header waits for the
    // payload so both go out in one datagram.
    const bool header = size == kTinCatHeader && get<uint32_t>(reinterpret_cast<const uint8_t*>(data), 0) == kTinCatMagic &&
        get<uint32_t>(reinterpret_cast<const uint8_t*>(data), 0x14) != 0;
    if (!header)
    {
        flush(s, nowMs());
    }
    return sent;
}

int UdpTransport::recv(Session& s, char* data, int size, bool nonBlocking)
{
    std::unique_lock lock(s.m);
    for (;;)
    {
        if (s.rxPos < s.rx.size())
        {
            break;
        }
        if (s.peerClosed)
        {
            return 0;
        }
        if (s.dead)
        {
            wsaError(WSAECONNRESET);
            return SOCKET_ERROR;
        }
        if (s.shut || s.localClosed)
        {
            wsaError(WSAESHUTDOWN);
            return SOCKET_ERROR;
        }
        if (s.status != Status::Open)
        {
            wsaError(WSAENOTCONN);
            return SOCKET_ERROR;
        }
        if (nonBlocking)
        {
            wsaError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        s.cv.wait(lock);
    }
    const size_t n = std::min(static_cast<size_t>(std::max(size, 0)), s.rx.size() - s.rxPos);
    std::memcpy(data, s.rx.data() + s.rxPos, n);
    s.rxPos += n;
    if (s.rxPos == s.rx.size())
    {
        s.rx.clear();
        s.rxPos = 0;
    }
    else if (s.rxPos >= 64 * 1024)
    {
        s.rx.erase(s.rx.begin(), s.rx.begin() + static_cast<ptrdiff_t>(s.rxPos));
        s.rxPos = 0;
    }
    return static_cast<int>(n);
}

bool UdpTransport::readable(Session& s)
{
    std::scoped_lock lock(s.m);
    return s.rxPos < s.rx.size() || s.peerClosed || s.dead || s.shut || s.status == Status::Failed;
}

bool UdpTransport::writable(Session& s)
{
    std::scoped_lock lock(s.m);
    return s.status == Status::Open && (s.dead || s.peerClosed || !s.kcp || ikcp_waitsnd(s.kcp) < kMaxQueued);
}

void UdpTransport::shutdown(Session& s)
{
    {
        std::scoped_lock lock(s.m);
        s.shut = true;
        s.cv.notify_all();
    }
    notifyChange();
}

void UdpTransport::close(const SessionPtr& s)
{
    {
        std::scoped_lock lock(s->m);
        if (!s->localClosed)
        {
            s->localClosed = true;
            s->closedAt = nowMs();
        }
        s->cv.notify_all();
    }
    notifyChange();
    UdpEndpoint::wake();
}

sockaddr_in UdpTransport::peer(Session& s)
{
    std::scoped_lock lock(s.m);
    return s.peer;
}

uint64_t UdpTransport::changes()
{
    return g_changes;
}

void UdpTransport::waitForChange(uint64_t seen, uint32_t ms)
{
    std::unique_lock lock(g_changeMutex);
    g_changeCv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return g_changes != seen; });
}
