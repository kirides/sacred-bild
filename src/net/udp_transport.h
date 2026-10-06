#pragma once
#include "net/address.h"

#include <cstdint>
#include <memory>

// The game connection over UDP ([Net] Udp): TinCat's TCP byte stream carried by KCP (reliable, ordered) between a
// player's sacred.exe and the host's gameserver.exe, on SacredBild's UDP endpoint (docs/UDP_PROTOCOL.md). Lost
// datagrams are resent after about one round trip instead of TCP's 300 ms and more, the resend delay doesn't
// double over an outage, and a player whose address changes (Wi-Fi <-> mobile, NAT rebinding) keeps the
// connection. Sessions run over IPv4 or IPv6, whichever answers first. TincatShim gives TinCat sockets backed by
// these sessions.
namespace UdpTransport
{
    struct Session;
    using SessionPtr = std::shared_ptr<Session>;

    enum class Status
    {
        Connecting,
        Open,
        Failed,         // no answer or refused: the player connects over TCP instead
    };

    // Registers the endpoint handlers. host: this is the gameserver; it accepts sessions if [Net] Udp is on and
    // refuses them otherwise (so players fall back to TCP right away).
    void install(bool host);

    // Player: starts the handshake with the gameserver at `tcpTarget` (the address the game connects to).
    // nullptr if the endpoint can't be opened.
    SessionPtr connect(const sockaddr_in& tcpTarget);
    Status status(Session& s);
    // Blocks until the handshake is done; true if it is open.
    bool waitConnected(Session& s);

    // Host: the TCP port TinCat listens on (0 = none); handshakes for another port are refused.
    void setListener(uint16_t tcpPort);
    bool hasPendingAccept();
    SessionPtr accept();

    // Like send() / recv(): bytes, or -1 with the Winsock error set.
    int send(Session& s, const char* data, int size, bool nonBlocking);
    int recv(Session& s, char* data, int size, bool nonBlocking);
    bool readable(Session& s);
    bool writable(Session& s);
    void shutdown(Session& s);
    // The socket is gone: data still queued is sent (up to 5 s), then the peer is told.
    void close(const SessionPtr& s);
    Net::Address peer(Session& s);
    // The peer for TinCat, which only takes IPv4: an IPv6 peer gets a stand-in address.
    sockaddr_in peerForGame(Session& s);

    // select(): a counter that changes whenever a session may have become readable or writable, and a wait for it.
    uint64_t changes();
    void waitForChange(uint64_t seen, uint32_t ms);
}
