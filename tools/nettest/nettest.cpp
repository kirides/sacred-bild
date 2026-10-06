// Test bench for the UDP transport and the matchmaker client, without the game. Each role runs as its own process
// (the transport has one role per process, like sacred.exe and gameserver.exe):
//
//   nettest host <udpPort> [matchmaker]             echoes everything players send (TCP port 6000 for the handshake)
//   nettest proxy <port> <hostPort> <loss%> <delayMs> [roamSec] [outageSec]
//                                                   lossy, delaying UDP proxy in front of the host; every roamSec it
//                                                   forwards from a new port (the host sees the player move); from
//                                                   3 s on it drops everything for outageSec
//   nettest client <udpPort> <seconds> [matchmaker] connects (to 127.0.0.1:<udpPort>, or to the matchmaker's game),
//                                                   sends TinCat-like messages, checks the echo, prints round trips
//
// Logs go to nettest-<role>.log next to the exe. Environment: NETTEST_PREFER=4 is [Net] Prefer=IPv4; NETTEST_DROP=4
// or 6 drops the game connection's datagrams arriving over that family (e.g. on the host: a broken IPv6 path).
#include "net/lan_client.h"
#include "net/lan_protocol.h"
#include "net/matchmaker.h"
#include "net/udp_endpoint.h"
#include "net/udp_transport.h"
#include "config.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint16_t kTcpPort = 6000;
    constexpr uint32_t kMagic = 0xDABAFBEF;
    uint16_t g_relayPort = 2105;

    uint64_t micros()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    }

    sockaddr_in loopback(uint16_t port)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        return a;
    }

    void initLog(const char* role)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p = path;
        p.resize(p.rfind(L'\\') + 1);
        p += L"nettest-";
        for (const char* c = role; *c; ++c)
        {
            p += static_cast<wchar_t>(*c);
        }
        p += L".log";
        Log::init(p.c_str());
    }

    // --- host ---

    void echo(UdpTransport::SessionPtr s)
    {
        std::vector<char> buf(64 * 1024);
        uint64_t total = 0;
        for (;;)
        {
            const int n = UdpTransport::recv(*s, buf.data(), static_cast<int>(buf.size()), false);
            if (n <= 0)
            {
                LOG("echo: recv {} (error {}) after {} bytes", n, WSAGetLastError(), total);
                break;
            }
            total += n;
            if (UdpTransport::send(*s, buf.data(), n, false) != n)
            {
                LOG("echo: send failed ({})", WSAGetLastError());
                break;
            }
        }
        UdpTransport::close(s);
    }

    int host(uint16_t port, const char* matchmaker)
    {
        g_config.netUdp = true;
        g_config.netPort = port;
        g_config.netMatchmaker = matchmaker ? matchmaker : "";
        UdpTransport::install(true);
        Matchmaker::install(true);
        if (!UdpEndpoint::open(port, true))
        {
            std::printf("host: port %u not available\n", port);
            return 1;
        }
        UdpTransport::setListener(kTcpPort);
        uint8_t plain[LanAnnounce::kSize] = {};
        const uint16_t tcpPort = kTcpPort;
        std::memcpy(plain + 2, &tcpPort, 2);
        plain[0xC] = 1;
        plain[0xD] = 4;
        const wchar_t name[] = L"nettest";
        std::memcpy(plain + 0xE, name, sizeof(name));
        std::printf("host: UDP %u\n", port);
        for (int i = 0;; ++i)
        {
            if (i % 30 == 0)
            {
                Matchmaker::publish(plain);
            }
            while (auto s = UdpTransport::accept())
            {
                std::thread(echo, s).detach();
            }
            UdpTransport::waitForChange(UdpTransport::changes(), 100);
        }
    }

    // --- proxy ---

    struct Delayed
    {
        uint64_t due;
        std::vector<char> data;
        bool toHost;
        bool operator>(const Delayed& o) const { return due > o.due; }
    };

    int proxy(uint16_t port, uint16_t hostPort, int loss, int delayMs, int roamSec, int outageSec)
    {
        // The outage: everything dropped from 3 s on, for outageSec.
        const uint64_t outageFrom = micros() + 3000000, outageTo = outageFrom + uint64_t(outageSec) * 1000000;
        SOCKET front = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local = loopback(port);
        if (bind(front, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
        {
            std::printf("proxy: port %u not available\n", port);
            return 1;
        }
        std::atomic<SOCKET> back = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in hostAddr = loopback(hostPort);
        sockaddr_in client{};
        std::mutex mutex;
        std::priority_queue<Delayed, std::vector<Delayed>, std::greater<>> queue;
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> percent(0, 99);
        uint64_t dropped = 0, forwarded = 0;

        const auto enqueue = [&](const char* data, int n, bool toHost) {
            std::scoped_lock lock(mutex);
            const uint64_t now = micros();
            if (percent(rng) < loss || (now >= outageFrom && now < outageTo))
            {
                ++dropped;
                return;
            }
            ++forwarded;
            queue.push({micros() + uint64_t(delayMs) * 1000, std::vector<char>(data, data + n), toHost});
        };
        std::thread([&] {
            char buf[2048];
            for (;;)
            {
                sockaddr_in from{};
                int len = sizeof(from);
                const int n = recvfrom(front, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
                if (n > 0)
                {
                    {
                        std::scoped_lock lock(mutex);
                        client = from;
                    }
                    enqueue(buf, n, true);
                }
            }
        }).detach();
        const auto readBack = [&](SOCKET s) {
            char buf[2048];
            for (;;)
            {
                const int n = recv(s, buf, sizeof(buf), 0);
                if (n == SOCKET_ERROR && WSAGetLastError() != WSAECONNRESET && WSAGetLastError() != WSAEMSGSIZE)
                {
                    return;     // closed by a roam
                }
                if (n > 0)
                {
                    enqueue(buf, n, false);
                }
            }
        };
        sendto(back, "", 0, 0, reinterpret_cast<const sockaddr*>(&hostAddr), sizeof(hostAddr));
        std::thread(readBack, SOCKET(back)).detach();
        std::printf("proxy: %u -> %u, %d%% loss, %d ms delay each way, roam every %d s\n", port, hostPort, loss, delayMs,
            roamSec);
        uint64_t nextRoam = roamSec ? micros() + uint64_t(roamSec) * 1000000 : UINT64_MAX;
        uint64_t nextReport = micros() + 5000000;
        for (;;)
        {
            std::vector<Delayed> due;
            {
                std::scoped_lock lock(mutex);
                while (!queue.empty() && queue.top().due <= micros())
                {
                    due.push_back(queue.top());
                    queue.pop();
                }
            }
            for (const auto& d : due)
            {
                if (d.toHost)
                {
                    sendto(back, d.data.data(), static_cast<int>(d.data.size()), 0,
                        reinterpret_cast<const sockaddr*>(&hostAddr), sizeof(hostAddr));
                }
                else
                {
                    sockaddr_in c;
                    {
                        std::scoped_lock lock(mutex);
                        c = client;
                    }
                    sendto(front, d.data.data(), static_cast<int>(d.data.size()), 0, reinterpret_cast<const sockaddr*>(&c),
                        sizeof(c));
                }
            }
            if (micros() >= nextRoam)
            {
                nextRoam += uint64_t(roamSec) * 1000000;
                const SOCKET old = back.exchange(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
                sendto(back, "", 0, 0, reinterpret_cast<const sockaddr*>(&hostAddr), sizeof(hostAddr));
                std::thread(readBack, SOCKET(back)).detach();
                closesocket(old);
                std::printf("proxy: roamed to a new port\n");
            }
            if (micros() >= nextReport)
            {
                nextReport += 5000000;
                std::scoped_lock lock(mutex);
                std::printf("proxy: %llu forwarded, %llu dropped\n", forwarded, dropped);
            }
            Sleep(1);
        }
    }

    // --- client ---

    int client(uint16_t udpPort, int seconds, const char* matchmaker)
    {
        g_config.netUdp = true;
        g_config.netMatchmaker = matchmaker ? matchmaker : "";
        g_relayPort = udpPort;
        UdpTransport::install(false);
        Matchmaker::install(false);

        sockaddr_in target = loopback(kTcpPort);
        if (matchmaker)
        {
            // Like the LAN list: poll until the game shows up, then connect to the address it was announced with.
            for (int i = 0;; ++i)
            {
                std::vector<Matchmaker::Listed> games;
                Matchmaker::poll(games);
                if (!games.empty())
                {
                    target.sin_addr.s_addr = games.front().address;
                    std::printf("client: matchmaker lists '%s', announced as %s\n",
                        LanAnnounce::gameName(games.front().plain).c_str(),
                        Net::toString(Net::ipv4(target.sin_addr.s_addr, kTcpPort)).c_str());
                    break;
                }
                if (i == 100)
                {
                    std::printf("client: FAIL: the matchmaker lists no game\n");
                    return 1;
                }
                Sleep(100);
            }
        }
        const auto s = UdpTransport::connect(target);
        if (!s || !UdpTransport::waitConnected(*s))
        {
            std::printf("client: FAIL: no connection\n");
            return 1;
        }
        std::printf("client: connected\n");
        std::atomic<uint32_t> received = 0;

        std::atomic<bool> done = false;
        std::atomic<uint32_t> sentCount = 0;
        std::vector<uint32_t> rtts;
        std::atomic<bool> ok = true;
        const uint64_t start = micros();

        // The payload of message `seq`: seq, send time, then bytes derived from seq.
        const auto payloadSize = [](uint32_t seq) -> uint32_t {
            return seq % 97 == 0 ? 120000 : 16 + (seq * 2654435761u) % 3000;
        };
        std::thread reader([&] {
            std::vector<char> buf;
            uint32_t expect = 0;
            const auto readAll = [&](char* p, int n) {
                while (n > 0)
                {
                    const int r = UdpTransport::recv(*s, p, n, false);
                    if (r <= 0)
                    {
                        return false;
                    }
                    p += r;
                    n -= r;
                }
                return true;
            };
            for (;;)
            {
                char header[0x1C];
                if (!readAll(header, sizeof(header)))
                {
                    if (!done)  // after the test: shutdown() ends the wait
                    {
                        std::printf("client: FAIL: connection lost (%d)\n", WSAGetLastError());
                        ok = false;
                    }
                    return;
                }
                uint32_t magic, size;
                std::memcpy(&magic, header, 4);
                std::memcpy(&size, header + 0x14, 4);
                buf.resize(size);
                if (magic != kMagic || !readAll(buf.data(), static_cast<int>(size)))
                {
                    std::printf("client: FAIL: bad stream\n");
                    ok = false;
                    return;
                }
                uint32_t seq;
                uint64_t sentAt;
                std::memcpy(&seq, buf.data(), 4);
                std::memcpy(&sentAt, buf.data() + 4, 8);
                bool intact = seq == expect && size == payloadSize(seq);
                for (uint32_t i = 12; intact && i < size; ++i)
                {
                    intact = static_cast<uint8_t>(buf[i]) == static_cast<uint8_t>(seq + i);
                }
                if (!intact)
                {
                    std::printf("client: FAIL: message %u out of order or damaged (expected %u)\n", seq, expect);
                    ok = false;
                    return;
                }
                ++expect;
                received = expect;
                rtts.push_back(static_cast<uint32_t>((micros() - sentAt) / 1000));
            }
        });

        std::thread([&] {
            while (!done)
            {
                Sleep(1000);
                std::printf("client: %u sent, %u echoed\n", sentCount.load(), received.load());
                std::fflush(stdout);
            }
        }).detach();

        // TinCat-like: a 28-byte header and the payload as two sends, ~50 messages a second.
        std::vector<char> payload;
        uint32_t seq = 0;
        while (micros() - start < uint64_t(seconds) * 1000000 && ok)
        {
            const uint32_t size = payloadSize(seq);
            payload.resize(size);
            const uint64_t now = micros();
            std::memcpy(payload.data(), &seq, 4);
            std::memcpy(payload.data() + 4, &now, 8);
            for (uint32_t i = 12; i < size; ++i)
            {
                payload[i] = static_cast<char>(seq + i);
            }
            char header[0x1C] = {};
            std::memcpy(header, &kMagic, 4);
            std::memcpy(header + 0x14, &size, 4);
            if (UdpTransport::send(*s, header, sizeof(header), false) != sizeof(header) ||
                UdpTransport::send(*s, payload.data(), static_cast<int>(size), false) != static_cast<int>(size))
            {
                std::printf("client: FAIL: send (%d)\n", WSAGetLastError());
                ok = false;
                break;
            }
            sentCount = ++seq;
            Sleep(20);
        }
        // Wait for the last echoes, then end the reader's wait.
        for (int i = 0; i < 1000 && ok && received < seq; ++i)
        {
            Sleep(10);
        }
        done = true;
        UdpTransport::shutdown(*s);
        reader.join();
        UdpTransport::close(s);
        if (ok && received != seq)
        {
            std::printf("client: FAIL: %u of %u messages echoed\n", received.load(), seq);
            ok = false;
        }
        Sleep(200);     // the CLOSE goes out on the next tick
        if (!ok)
        {
            return 1;
        }
        std::sort(rtts.begin(), rtts.end());
        const auto pct = [&](double p) { return rtts[std::min(rtts.size() - 1, size_t(p * rtts.size()))]; };
        std::printf("client: OK: %u messages echoed intact and in order; round trip ms: median %u, p95 %u, p99 %u, "
            "max %u\n", seq, pct(0.5), pct(0.95), pct(0.99), rtts.back());
        return 0;
    }
}

uint16_t LanClient::relayPort(uint32_t)
{
    return g_relayPort;
}

int main(int argc, char** argv)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    setvbuf(stdout, nullptr, _IONBF, 0);
    timeBeginPeriod(1);     // the proxy's delays and the 20 ms send interval at 1 ms, not 15.6 ms
    const std::string role = argc > 1 ? argv[1] : "";
    initLog(role.c_str());
    // NETTEST_PREFER=4: [Net] Prefer=IPv4. NETTEST_DROP=4 or 6: the game connection over that family doesn't get through.
    if (const char* prefer = std::getenv("NETTEST_PREFER"))
    {
        g_config.netPreferIpv6 = std::strcmp(prefer, "4") != 0;
    }
    if (const char* drop = std::getenv("NETTEST_DROP"))
    {
        UdpEndpoint::dropReceived(std::strcmp(drop, "6") == 0 ? AF_INET6 : AF_INET);
    }
    if (role == "host" && argc >= 3)
    {
        return host(static_cast<uint16_t>(std::atoi(argv[2])), argc >= 4 ? argv[3] : nullptr);
    }
    if (role == "proxy" && argc >= 6)
    {
        return proxy(static_cast<uint16_t>(std::atoi(argv[2])), static_cast<uint16_t>(std::atoi(argv[3])),
            std::atoi(argv[4]), std::atoi(argv[5]), argc >= 7 ? std::atoi(argv[6]) : 0,
            argc >= 8 ? std::atoi(argv[7]) : 0);
    }
    if (role == "client" && argc >= 4)
    {
        return client(static_cast<uint16_t>(std::atoi(argv[2])), std::atoi(argv[3]), argc >= 5 ? argv[4] : nullptr);
    }
    std::printf("usage: see the comment at the top of tools/nettest/nettest.cpp\n");
    return 2;
}
