#pragma once
#include <cstdint>
#include <string>

// Realms: the online play (register / login) server the game talks to.
//
// The network menu runs cNet_openScreen when a network screen opens. It reads Settings.cfg into the network object
// (cNet_loadSettings: the lobby host at +0x39E4, the port at +0x3C), then makes the lobby connection, which reads
// the port again. The login and register calls copy the host from the object every time. Gear hooks those steps:
// when the player opens Closed or Open Internet and a realm is chosen, the realm's host replaces the one from
// Settings.cfg and its port the one the lobby connection is made with. LAN is not touched. The choice is kept in
// mods\gear_realm.txt (a native file, so it applies before Lua runs: mods load with the first in-game frame).
//
// Engine thread only.
namespace Realm
{
    struct State
    {
        bool available = false;     // the network object exists
        std::string host;           // its lobby host now
        uint32_t port = 0;          // its lobby port (what Settings.cfg said, or the realm's)
        bool connected = false;     // a lobby connection is open
        std::string gameHost;       // what Settings.cfg says (known once the network menu has opened)
        bool overridden = false;    // a realm is chosen (host and port of the next internet menu come from it)
    };

    // Installs the hooks and reads the saved choice from `path`. Inside the Patch transaction.
    bool install(const std::wstring& path);

    // Chooses the realm: `host` empty clears it (the game's Settings.cfg applies again). `port` 0: the game's port.
    // Saved at once; an existing network object that has no lobby connection open gets it too. false with `error`.
    bool select(const std::string& host, uint32_t port, const char*& error);

    State read();

    // Whether `host` is acceptable (1..120 characters of letters, digits, dot, dash, underscore).
    bool validHost(const std::string& host);
}
