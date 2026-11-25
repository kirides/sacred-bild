// The Lua host's natives for the network: kills, mod frames and net channels, chat, multiplayer state, realms.
// Split out of lua_host.cpp; the shared helpers are in lua_native.h.
#include "lua_host.h"
#include "automation.h"
#include "chat.h"
#include "world/classes.h"
#include "items/itemuse.h"
#include "items/drops.h"
#include "gamecfg.h"
#include "gear.h"
#include "instance.h"
#include "items/inventory.h"
#include "items/itemtip.h"
#include "items/itemicon.h"
#include "ui/keyblock.h"
#include "world/killstat.h"
#include "ui/input.h"
#include "log.h"
#include "netmsg.h"
#include "items/pickup.h"
#include "items/potionpick.h"
#include "items/shop.h"
#include "realm.h"
#include "ui/replace.h"
#include "ui/rml.h"
#include "overlay.h"
#include "world/buffs.h"
#include "world/pfx.h"
#include "world/party.h"
#include "ui/padnav.h"
#include "pipe.h"
#include "save.h"
#include "savegames.h"
#include "sitehook.h"
#include "items/master.h"
#include "tables.h"
#include "engine/gear_addr.h"
#include "ui/ui.h"
#include "world/world.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <lua.hpp>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "lua_native.h"

namespace LuaNative
{
    // native.net_kill() -> nil | {victim, killer, award}: the next kill this client sent (src/gear/netmsg.cpp); killer
    // is the hero object id that got the experience, 0 for none.
    int nativeNetKill(lua_State* L)
    {
        NetMsg::Kill k;
        if (!NetMsg::takeKill(k))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, k.victim);
        lua_setfield(L, -2, "victim");
        lua_pushnumber(L, k.killer);
        lua_setfield(L, -2, "killer");
        lua_pushnumber(L, k.award);
        lua_setfield(L, -2, "award");
        return 1;
    }

    // native.mp_send_frame(slot, utf16le) -> bool: one frame of the mod channel (the kernel frames, paces, reassembles).
    int nativeMpSendFrame(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1);
        size_t n = 0;
        const char* s = luaL_checklstring(L, 2, &n);
        uint16_t units[NetMsg::kMaxFrameUnits];
        const size_t count = n / 2;
        bool ok = n % 2 == 0 && count >= 1 && count <= NetMsg::kMaxFrameUnits && slot >= 0 && slot <= 16;
        if (ok)
        {
            std::memcpy(units, s, count * 2);
            ok = NetMsg::sendFrame(static_cast<uint32_t>(slot), units, count);
        }
        lua_pushboolean(L, ok);
        return 1;
    }

    // native.net_ops() -> control, data, ready: the custom opcode ids and whether frames can go out.
    int nativeNetOps(lua_State* L)
    {
        lua_pushnumber(L, NetMsg::controlOpcode());
        lua_pushnumber(L, NetMsg::dataOpcode());
        lua_pushboolean(L, NetMsg::opsReady());
        return 3;
    }

    // native.net_configure(control, data) -> bool: set the opcode ids (before the first frame is sent).
    int nativeNetConfigure(lua_State* L)
    {
        const lua_Number c = luaL_checknumber(L, 1), d = luaL_checknumber(L, 2);
        lua_pushboolean(L, c >= 0 && c < 0x10000 && d >= 0 && d < 0x10000 &&
                               NetMsg::setOpcodes(static_cast<uint16_t>(c), static_cast<uint16_t>(d)));
        return 1;
    }

    // native.net_send_op(type, bytes) -> bool: one message of a custom opcode to the game server.
    int nativeNetSendOp(lua_State* L)
    {
        const lua_Number type = luaL_checknumber(L, 1);
        size_t n = 0;
        const char* s = luaL_checklstring(L, 2, &n);
        lua_pushboolean(L, type >= 0 && type < 0x10000 && NetMsg::sendOp(static_cast<uint16_t>(type), s, n));
        return 1;
    }

    // native.net_take_op() -> type, sender slot, bytes | nil: the oldest custom opcode message received.
    int nativeNetTakeOp(lua_State* L)
    {
        NetMsg::OpFrame f;
        if (!NetMsg::takeOp(f))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, f.type);
        lua_pushnumber(L, f.sender);
        lua_pushlstring(L, reinterpret_cast<const char*>(f.data.data()), f.data.size());
        return 3;
    }

    // native.hero_saved() -> slot, network | nil: a hero file the game wrote.
    int nativeHeroSaved(lua_State* L)
    {
        NetMsg::HeroSaved h;
        if (!NetMsg::takeHeroSaved(h))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, h.slot);
        lua_pushboolean(L, h.network);
        return 2;
    }

    // native.chat_take() -> {text, color, channel, kind, main, hidden, slot?, sender?, message?} | nil: the oldest chat or
    // message line the game added (src/gear/chat.cpp).
    int nativeChatTake(lua_State* L)
    {
        static const char* const kKinds[] = {"system", "global", "team", "private", "server", "own", "notice"};
        Chat::Line line;
        if (!Chat::take(line))
        {
            lua_pushnil(L);
            return 1;
        }
        const std::string text = narrow(line.text);
        const std::string sender = narrow(line.sender);
        const std::string message = narrow(line.message);
        const size_t kind = static_cast<size_t>(line.kind) < std::size(kKinds) ? static_cast<size_t>(line.kind) : 0;
        const bool paired = !line.message.empty();
        lua_createtable(L, 0, 9);
        lua_pushlstring(L, text.data(), text.size());
        lua_setfield(L, -2, "text");
        lua_pushnumber(L, line.color);
        lua_setfield(L, -2, "color");
        lua_pushnumber(L, line.channel);
        lua_setfield(L, -2, "channel");
        lua_pushstring(L, kKinds[kind]);
        lua_setfield(L, -2, "kind");
        lua_pushboolean(L, line.main);
        lua_setfield(L, -2, "main");
        lua_pushboolean(L, line.hidden);
        lua_setfield(L, -2, "hidden");
        if (paired)
        {
            lua_pushnumber(L, line.slot);
            lua_setfield(L, -2, "slot");
            if (!sender.empty())
            {
                lua_pushlstring(L, sender.data(), sender.size());
                lua_setfield(L, -2, "sender");
            }
            lua_pushlstring(L, message.data(), message.size());
            lua_setfield(L, -2, "message");
        }
        return 1;
    }

    // native.chat_hide(on): some mod shows the lines itself (refreshed every tick; see Chat::setHidden).
    int nativeChatHide(lua_State* L)
    {
        Chat::setHidden(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.chat_info() -> available, hidden
    int nativeChatInfo(lua_State* L)
    {
        lua_pushboolean(L, Chat::available());
        lua_pushboolean(L, Chat::hidden());
        return 2;
    }

    // native.item_color(id) -> ARGB | nil: the game's label colour of a live item.
    int nativeItemColor(lua_State* L)
    {
        const lua_Number id = luaL_checknumber(L, 1);
        uint32_t color = 0;
        if (id < 1 || id > 4294967295.0 || !Pickup::labelColor(static_cast<uint32_t>(id), color))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, color);
        return 1;
    }

    // native.mp_info() -> {network, is_host, host_slot (nil if unknown)}
    int nativeMpInfo(lua_State* L)
    {
        const NetMsg::Host h = NetMsg::host();
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, h.network);
        lua_setfield(L, -2, "network");
        lua_pushboolean(L, h.isHost);
        lua_setfield(L, -2, "is_host");
        if (h.hostSlot)
        {
            lua_pushnumber(L, h.hostSlot);
            lua_setfield(L, -2, "host_slot");
        }
        return 1;
    }

    // native.mp_mode() -> 'single' | 'lan' | 'open' | 'closed'
    int nativeMpMode(lua_State* L)
    {
        switch (World::networkMode())
        {
        case World::NetworkMode::Lan: lua_pushstring(L, "lan"); break;
        case World::NetworkMode::Open: lua_pushstring(L, "open"); break;
        case World::NetworkMode::Closed: lua_pushstring(L, "closed"); break;
        default: lua_pushstring(L, "single"); break;
        }
        return 1;
    }

    // native.auto_network_mode(mode) -> bool (self tests: sets the flags the kind of game is read from)
    int nativeAutoNetworkMode(lua_State* L)
    {
        const char* m = luaL_checkstring(L, 1);
        World::NetworkMode mode = World::NetworkMode::Single;
        if (!std::strcmp(m, "lan")) mode = World::NetworkMode::Lan;
        else if (!std::strcmp(m, "open")) mode = World::NetworkMode::Open;
        else if (!std::strcmp(m, "closed")) mode = World::NetworkMode::Closed;
        const bool internetUnproven = !std::strcmp(m, "internet");     // the Internet menu, nothing proven: reads as closed
        if (internetUnproven) mode = World::NetworkMode::Open;
        lua_pushboolean(L, World::setNetworkModeForTest(mode, !internetUnproven));
        return 1;
    }

    // native.auto_hero_from_server(on) -> nil (self tests: the hero came from the server, or did not)
    int nativeAutoHeroFromServer(lua_State* L)
    {
        NetMsg::setHeroFromServerForTest(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.auto_chat_add(text, channel, color) -> bool (self tests: a line on the game's message list)
    int nativeAutoChatAdd(lua_State* L)
    {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        const lua_Number channel = luaL_checknumber(L, 2), color = luaL_optnumber(L, 3, 0);
        const bool ok = channel >= 0 && channel < 0x10000 && color >= 0 && color <= 4294967295.0 &&
                        Chat::addForTest(widen(s, n), static_cast<uint16_t>(channel), static_cast<uint32_t>(color));
        lua_pushboolean(L, ok);
        return 1;
    }

    // native.auto_chat_send(type, target, text) -> bool (self tests: a chat message on the network)
    int nativeAutoChatSend(lua_State* L)
    {
        const lua_Number type = luaL_checknumber(L, 1), target = luaL_checknumber(L, 2);
        size_t n = 0;
        const char* s = luaL_checklstring(L, 3, &n);
        const std::wstring w = widen(s, n);
        const bool ok = type >= 3 && type <= 5 && target >= 0 && target <= 16 &&
                        NetMsg::sendChatForTest(static_cast<uint16_t>(type), static_cast<uint32_t>(target),
                                                reinterpret_cast<const uint16_t*>(w.data()), w.size());
        lua_pushboolean(L, ok);
        return 1;
    }

    // native.hero_name(slot) -> UTF-8 | nil
    int nativeHeroName(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1);
        if (!(slot >= 1 && slot <= 16))
        {
            lua_pushnil(L);
            return 1;
        }
        const std::wstring w = NetMsg::heroName(static_cast<uint32_t>(slot));
        char utf8[512];
        const int n = w.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), utf8, sizeof(utf8), nullptr, nullptr);
        if (n <= 0)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, utf8, static_cast<size_t>(n));
        return 1;
    }

    // native.mp_take_frame() -> slot, utf16le | nil: the oldest frame another client sent this one.
    int nativeMpTakeFrame(lua_State* L)
    {
        uint32_t slot = 0;
        std::vector<uint16_t> units;
        if (!NetMsg::takeFrame(slot, units))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, slot);
        lua_pushlstring(L, reinterpret_cast<const char*>(units.data()), units.size() * 2);
        return 2;
    }

    // native.realm_get() -> {host, port, connected, overridden, game_host} | nil (the game has not made its network object yet)
    int nativeRealmGet(lua_State* L)
    {
        const Realm::State s = Realm::read();
        if (!s.available)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, s.host.data(), s.host.size());
        lua_setfield(L, -2, "host");
        lua_pushnumber(L, s.port);
        lua_setfield(L, -2, "port");
        lua_pushboolean(L, s.connected);
        lua_setfield(L, -2, "connected");
        lua_pushboolean(L, s.overridden);
        lua_setfield(L, -2, "overridden");
        lua_pushlstring(L, s.gameHost.data(), s.gameHost.size());
        lua_setfield(L, -2, "game_host");
        return 1;
    }

    // native.realm_select(host?, port?) -> true | nil, message. No host: the game's own (Settings.cfg) applies again.
    int nativeRealmSelect(lua_State* L)
    {
        size_t n = 0;
        const char* host = lua_isstring(L, 1) ? lua_tolstring(L, 1, &n) : "";
        const lua_Number port = lua_isnumber(L, 2) ? lua_tonumber(L, 2) : 0;
        const char* error = nullptr;
        if (port >= 0 && port <= 65535 && Realm::select(std::string(host, n), static_cast<uint32_t>(port), error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushstring(L, error ? error : "port: 1 to 65535");
        return 2;
    }

    // native.realm_valid_host(host) -> bool
    int nativeRealmValidHost(lua_State* L)
    {
        size_t n = 0;
        const char* host = luaL_checklstring(L, 1, &n);
        lua_pushboolean(L, Realm::validHost(std::string(host, n)));
        return 1;
    }


    void registerNet(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"net_kill", nativeNetKill},
            {"mp_send_frame", nativeMpSendFrame},
            {"net_ops", nativeNetOps},
            {"net_configure", nativeNetConfigure},
            {"net_send_op", nativeNetSendOp},
            {"net_take_op", nativeNetTakeOp},
            {"hero_saved", nativeHeroSaved},
            {"chat_take", nativeChatTake},
            {"chat_hide", nativeChatHide},
            {"chat_info", nativeChatInfo},
            {"item_color", nativeItemColor},
            {"mp_info", nativeMpInfo},
            {"mp_mode", nativeMpMode},
            {"hero_name", nativeHeroName},
            {"mp_take_frame", nativeMpTakeFrame},
            {"realm_get", nativeRealmGet},
            {"realm_select", nativeRealmSelect},
            {"realm_valid_host", nativeRealmValidHost},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        static constexpr luaL_Reg kAutomation[] = {
            {"auto_network_mode", nativeAutoNetworkMode},
            {"auto_hero_from_server", nativeAutoHeroFromServer},
            {"auto_chat_add", nativeAutoChatAdd},
            {"auto_chat_send", nativeAutoChatSend},
            {nullptr, nullptr},
        };
        if (automation)
        {
            setFunctions(L, kAutomation);
        }
    }
}
