#include "realm.h"
#include "log.h"
#include "patch.h"
#include "engine/gear_addr.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Realm
{
    namespace
    {
        constexpr size_t kIp = 0x38;
        constexpr size_t kPort = 0x3C;
        constexpr size_t kHost = 0x39E4;
        constexpr size_t kMaxHost = 120;

        std::mutex g_lock;
        std::string g_host;             // empty: no realm chosen
        uint32_t g_port = 0;            // 0: the game's
        std::wstring g_path;
        bool g_inOpen = false;          // inside cNet_openScreen
        uint32_t g_openFlag = 0;

        decltype(Gear::Addr::cNet_openScreen)::Ptr g_origOpen = nullptr;
        decltype(Gear::Addr::cNet_loadSettings)::Ptr g_origLoad = nullptr;
        decltype(Gear::Addr::cNet_saveSettings)::Ptr g_origSave = nullptr;
        std::string g_gameHost;         // what Settings.cfg says (read by the game's own load)
        uint32_t g_gamePort = 0;
        using PortFn = uint32_t(__fastcall*)(void* config, void* edx);
        PortFn g_origPort = nullptr;

        bool lobbyConnected(const uint8_t* lobby)
        {
            return lobby && *reinterpret_cast<const uint32_t*>(lobby + 0x10) == 1 && lobby[8] != 0;
        }

        // No C++ objects in the functions that touch the game's memory (__try).
        bool readFields(uint8_t* net, uint8_t* lobby, char* host, size_t hostSize, uint32_t* port, uint32_t* ip,
                        bool* connected, ptrdiff_t* span)
        {
            __try
            {
                *ip = *reinterpret_cast<uint32_t*>(net + kIp);
                *port = *reinterpret_cast<uint32_t*>(net + kPort);
                const char* begin = *reinterpret_cast<const char**>(net + kHost);
                const char* end = *reinterpret_cast<const char**>(net + kHost + 4);
                size_t n = 0;
                if (begin)
                {
                    while (n + 1 < hostSize && begin[n])
                    {
                        host[n] = begin[n];
                        ++n;
                    }
                }
                host[n] = 0;
                *span = begin ? end - begin : 0;
                *connected = lobbyConnected(lobby);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool assignHost(uint8_t* net, const char* host, size_t n)
        {
            __try
            {
                char allocator = 0;
                Gear::Addr::cString_assign(net + kHost, host, host + n, &allocator);
                *reinterpret_cast<uint32_t*>(net + kIp) = 0;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool setPort(uint8_t* net, uint32_t port)
        {
            __try
            {
                *reinterpret_cast<uint32_t*>(net + kPort) = port;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        uint8_t* network() { return Gear::Addr::g_pNetPlayers ? *Gear::Addr::g_pNetPlayers : nullptr; }

        // Puts the chosen realm into the network object; false with `error` if it did not take.
        bool apply(uint8_t* net, const std::string& host, uint32_t port, const char*& error)
        {
            uint8_t* lobby = Gear::Addr::g_pLobby ? *Gear::Addr::g_pLobby : nullptr;
            char current[kMaxHost + 8];
            uint32_t curPort = 0, ip = 0;
            bool connected = false;
            ptrdiff_t span = 0;
            if (!readFields(net, lobby, current, sizeof current, &curPort, &ip, &connected, &span))
            {
                error = "the network object's host field cannot be read (layout mismatch)";
                return false;
            }
            // The game's strings are begin / end / capacity with the terminating zero at `end` (outside begin..end; the
            // assign moves it along): anything else means the layout is not what this code was written for.
            if (span != static_cast<ptrdiff_t>(std::strlen(current)))
            {
                error = "the host string does not have the expected layout";
                return false;
            }
            if (!assignHost(net, host.c_str(), host.size()))
            {
                error = "assigning the host failed";
                return false;
            }
            if (port && !setPort(net, port))
            {
                error = "setting the port failed";
                return false;
            }
            char after[kMaxHost + 8];
            uint32_t afterPort = 0, afterIp = 0;
            bool afterConnected = false;
            ptrdiff_t afterSpan = 0;
            if (!readFields(net, lobby, after, sizeof after, &afterPort, &afterIp, &afterConnected, &afterSpan) ||
                host != after)
            {
                error = "the host did not take (read back differently)";
                return false;
            }
            return true;
        }

        void __fastcall hookOpenScreen(void* net, void* edx, uint32_t flag)
        {
            const bool outer = !g_inOpen;
            g_inOpen = true;
            g_openFlag = flag;
            g_origOpen(net, edx, flag);
            if (outer)
            {
                g_inOpen = false;
            }
        }

        void __fastcall hookLoadSettings(void* net, void* edx)
        {
            g_origLoad(net, edx);
            {
                // Settings.cfg's own lobby, kept so that the file is written back unchanged (hookSaveSettings).
                char own[kMaxHost + 8];
                uint32_t ownPort = 0, ip = 0;
                bool connected = false;
                ptrdiff_t span = 0;
                if (readFields(static_cast<uint8_t*>(net), nullptr, own, sizeof own, &ownPort, &ip, &connected, &span) &&
                    own[0] != 0)
                {
                    std::lock_guard lock(g_lock);
                    g_gameHost = own;
                    g_gamePort = ownPort;
                }
            }
            std::string host;
            uint32_t port = 0;
            {
                std::lock_guard lock(g_lock);
                host = g_host;
                port = g_port;
            }
            if (!g_inOpen)
            {
                return;
            }
            LOG("Realm: network screen opened (flag {}), realm {}", g_openFlag, host.empty() ? "none" : host);
            if (host.empty() || g_openFlag != 1)
            {
                return;
            }
            const char* error = nullptr;
            if (apply(static_cast<uint8_t*>(net), host, port, error))
            {
                LOG("Realm: lobby is now {} port {}", host, port);
            }
            else
            {
                LOG("Realm: not applied: {}", error);
            }
        }

        // The object writes its lobby fields back into Settings.cfg now and then (login, register, the menu): the file
        // keeps the game's own lobby, not the realm's, so choosing the default realm again really is the default.
        void __fastcall hookSaveSettings(void* net, void* edx)
        {
            std::string own, chosen;
            uint32_t ownPort = 0, chosenPort = 0;
            {
                std::lock_guard lock(g_lock);
                own = g_gameHost;
                ownPort = g_gamePort;
                chosen = g_host;
                chosenPort = g_port;
            }
            const char* error = nullptr;
            const bool swap = !chosen.empty() && !own.empty() && apply(static_cast<uint8_t*>(net), own, ownPort, error);
            g_origSave(net, edx);
            if (swap && !apply(static_cast<uint8_t*>(net), chosen, chosenPort ? chosenPort : ownPort, error))
            {
                LOG("Realm: the realm could not be put back after writing Settings.cfg: {}", error);
            }
        }

        uint32_t __fastcall hookPort(void* config, void* edx)
        {
            uint32_t port = 0;
            {
                std::lock_guard lock(g_lock);
                port = g_host.empty() ? 0 : g_port;
            }
            if (g_inOpen && g_openFlag == 1 && port)
            {
                LOG("Realm: lobby port {}", port);
                return port;
            }
            return g_origPort(config, edx);
        }

        void save()
        {
            if (g_path.empty())
            {
                return;
            }
            std::string text;
            {
                std::lock_guard lock(g_lock);
                if (!g_host.empty())
                {
                    text = g_host + "\n" + std::to_string(g_port) + "\n";
                }
            }
            const std::wstring temp = g_path + L".tmp";
            HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            bool ok = f != INVALID_HANDLE_VALUE &&
                      (text.empty() || WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) &&
                      written == text.size();
            if (f != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f);
            }
            ok = ok && MoveFileExW(temp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!ok)
            {
                DeleteFileW(temp.c_str());
                LOG("Realm: gear_realm.txt cannot be written");
            }
        }

        void load()
        {
            HANDLE f = CreateFileW(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
            {
                return;
            }
            char buf[512];
            DWORD n = 0;
            ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
            CloseHandle(f);
            buf[n] = 0;
            std::string text(buf, n), host;
            const size_t eol = text.find_first_of("\r\n");
            host = text.substr(0, eol);
            const unsigned long port = eol == std::string::npos ? 0 : std::strtoul(text.c_str() + eol, nullptr, 10);
            if (validHost(host) && port <= 65535)
            {
                g_host = host;
                g_port = static_cast<uint32_t>(port);
                LOG("Realm: {} port {} chosen (gear_realm.txt)", g_host, g_port);
            }
        }
    }

    bool validHost(const std::string& host)
    {
        if (host.empty() || host.size() > kMaxHost)
        {
            return false;
        }
        for (const char ch : host)
        {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                            ch == '.' || ch == '-' || ch == '_';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    bool install(const std::wstring& path)
    {
        namespace A = Gear::Addr;
        g_path = path;
        load();
        if (!A::cNet_openScreen || !A::cNet_loadSettings || !A::cString_assign || !A::g_pNetPlayers)
        {
            LOG("Realm: the network menu code was not found: realms are off");
            return false;
        }
        bool ok = Patch::hook(g_origOpen, A::cNet_openScreen, hookOpenScreen, "cNet_openScreen");
        ok = Patch::hook(g_origLoad, A::cNet_loadSettings, hookLoadSettings, "cNet_loadSettings") && ok;
        if (A::cNet_saveSettings)
        {
            ok = Patch::hook(g_origSave, A::cNet_saveSettings, hookSaveSettings, "cNet_saveSettings") && ok;
        }
        if (A::lobbyPortCall && *reinterpret_cast<const uint8_t*>(A::lobbyPortCall) == 0xE8)
        {
            const uintptr_t site = A::lobbyPortCall;
            g_origPort = reinterpret_cast<PortFn>(site + 5 + *reinterpret_cast<const int32_t*>(site + 1));
            ok = Patch::redirectCall(site, reinterpret_cast<const void*>(&hookPort)) && ok;
        }
        else
        {
            LOG("Realm: the lobby port call was not found: a realm's port is not applied");
        }
        return ok;
    }

    State read()
    {
        State s;
        {
            std::lock_guard lock(g_lock);
            s.overridden = !g_host.empty();
            s.gameHost = g_gameHost;
        }
        uint8_t* net = network();
        if (!net)
        {
            return s;
        }
        uint8_t* lobby = Gear::Addr::g_pLobby ? *Gear::Addr::g_pLobby : nullptr;
        char host[kMaxHost + 8];
        uint32_t ip = 0;
        ptrdiff_t span = 0;
        if (readFields(net, lobby, host, sizeof host, &s.port, &ip, &s.connected, &span))
        {
            s.host = host;
            s.available = true;
        }
        return s;
    }

    bool select(const std::string& host, uint32_t port, const char*& error)
    {
        if (!host.empty() && !validHost(host))
        {
            error = "not a valid host name (letters, digits, '.', '-', '_', at most 120 characters)";
            return false;
        }
        if (port > 65535)
        {
            error = "port: 1 to 65535";
            return false;
        }
        {
            std::lock_guard lock(g_lock);
            g_host = host;
            g_port = host.empty() ? 0 : port;
        }
        save();
        // An existing network object: when it holds no lobby connection it gets the host now (the menu's next
        // opening would do it too); an open connection keeps its server until the player leaves.
        uint8_t* net = network();
        if (net && !host.empty())
        {
            const State now = read();
            // (An object whose settings the menu has not loaded yet has no host: the menu's own opening applies it.)
            if (now.available && !now.connected && !now.host.empty())
            {
                const char* applyError = nullptr;
                if (!apply(net, host, port, applyError))
                {
                    LOG("Realm: not applied to the network object: {}", applyError);
                }
            }
        }
        return true;
    }
}
