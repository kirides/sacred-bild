// The Lua host's natives for automation runs (registered only with -gear-auto) and the UI clip and wheel.
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
    // Automation (only registered when it is on, see start()).
    int nativeAutoInstance(lua_State* L)
    {
        const std::string& name = Automation::options().instance;
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    int nativeAutoMods(lua_State* L)
    {
        const auto& mods = Automation::options().mods;
        lua_createtable(L, static_cast<int>(mods.size()), 0);
        for (size_t i = 0; i < mods.size(); ++i)
        {
            lua_pushlstring(L, mods[i].data(), mods[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.auto_state() -> {menus, in_game, cinematic, help, dialog, ui, window, width, height, mouse_x, mouse_y}
    int nativeAutoState(lua_State* L)
    {
        const Automation::UiState st = Automation::uiState();
        int width = 0, height = 0;
        Automation::clientSize(width, height);
        lua_createtable(L, 0, 12);
        auto flag = [L](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
        auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
        flag("ui", st.available);
        flag("menus", st.menus);
        flag("in_game", st.inGame);
        flag("cinematic", st.cinematic);
        flag("help", st.help);
        flag("dialog", st.dialog);
        flag("window", Automation::window() != nullptr);
        num("width", width);
        num("height", height);
        num("mouse_x", st.mouseX);
        num("mouse_y", st.mouseY);
        return 1;
    }

    // native.auto_ui() -> {{group, index, parent, vtable, flags, x, y, width, height}, ...}; parent is a 1-based
    // index into the list, 0 for top level.
    int nativeAutoUi(lua_State* L)
    {
        const std::vector<Automation::UiNode> nodes = Automation::uiTree();
        lua_createtable(L, static_cast<int>(nodes.size()), 0);
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const Automation::UiNode& n = nodes[i];
            lua_createtable(L, 0, 9);
            lua_pushstring(L, n.group.c_str());
            lua_setfield(L, -2, "group");
            auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
            num("index", n.index);
            num("parent", n.parent + 1);
            num("vtable", n.vtable);
            num("flags", n.flags);
            num("x", n.x);
            num("y", n.y);
            num("width", n.width);
            num("height", n.height);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    int nativeAutoMove(lua_State* L)
    {
        const lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
        if (x > -1e5 && x < 1e5 && y > -1e5 && y < 1e5)
        {
            Automation::mouseMove(static_cast<int>(x), static_cast<int>(y));
        }
        return 0;
    }

    // native.auto_button(vk, down), native.auto_key(vk, down), native.auto_char(code)
    int nativeAutoButton(lua_State* L)
    {
        const int vk = static_cast<int>(luaL_checknumber(L, 1));
        Automation::button(vk, lua_toboolean(L, 2) != 0);
        return 0;
    }

    // native.auto_wheel(delta)
    int nativeAutoWheel(lua_State* L)
    {
        Automation::wheel(static_cast<int>(luaL_checknumber(L, 1)));
        return 0;
    }

    // native.ui_clip(x, y, w, h) sets the clip for what is recorded next; native.ui_clip() clears it.
    int nativeUiClip(lua_State* L)
    {
        if (lua_isnoneornil(L, 1))
        {
            Ui::setClip(false);
            return 0;
        }
        Ui::setClip(true, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                    static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)));
        return 0;
    }

    // native.ui_wheel() -> delta (WM_MOUSEWHEEL units over mod UI since the last call), pad scroll lines (+ down)
    int nativeUiWheel(lua_State* L)
    {
        lua_pushnumber(L, Ui::takeWheel());
        lua_pushnumber(L, Ui::takePadScroll());
        return 2;
    }

    int nativeAutoKey(lua_State* L)
    {
        const int vk = static_cast<int>(luaL_checknumber(L, 1));
        Automation::key(vk, lua_toboolean(L, 2) != 0);
        return 0;
    }

    int nativeAutoChar(lua_State* L)
    {
        const lua_Number c = luaL_checknumber(L, 1);
        if (c > 0 && c < 0x10000)
        {
            Automation::character(static_cast<wchar_t>(c));
        }
        return 0;
    }

    int nativeAutoRelease(lua_State*)
    {
        Automation::releaseAll();
        return 0;
    }

    int nativeAutoLoad(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1);
        const char* error = "the savegame number is 0..99";
        if (!(slot >= 0 && slot <= 99) || !Automation::loadSave(static_cast<int>(slot), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // Files automation writes go into <game>\GearAutomation\<instance>, named by the mod: letters, digits, _ - and one dot.
    std::wstring automationFile(const char* name, size_t n)
    {
        const std::string s(name, n);
        if (s.empty() || s.size() > 100 || s.find("..") != std::string::npos || s[0] == '.')
        {
            return {};
        }
        for (char c : s)
        {
            if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            {
                return {};
            }
        }
        // The run's own folder: parallel runs never write the same file.
        const std::wstring dir = Instance::directory(g_modsDir.substr(0, g_modsDir.size() - 5));
        if (dir.empty())
        {
            return {};
        }
        return dir + L"\\" + widen(s.data(), s.size());
    }

    // native.auto_screenshot(name) -> true | nil, message (a .bmp)
    int nativeAutoScreenshot(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const char* error = "bad file name";
        bool ok = false;
        {
            const std::wstring path = automationFile(name, n);
            ok = !path.empty() && Automation::screenshot(path, error);
        }
        if (!ok)
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    bool readObjectBytes(uintptr_t address, uint8_t* out, size_t size)
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // native.auto_object_bytes(id, offset, size) -> hex string | nil, message: raw bytes of an object (self tests and RE; at most 4096)
    int nativeAutoObjectBytes(lua_State* L)
    {
        const uintptr_t object = World::objectAddress(checkId(L, 1));
        const lua_Integer offset = luaL_checkinteger(L, 2);
        const lua_Integer size = luaL_checkinteger(L, 3);
        if (!object || offset < 0 || size < 1 || size > 4096)
        {
            lua_pushnil(L);
            lua_pushstring(L, "no such object, or the range is not 1..4096 bytes");
            return 2;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!readObjectBytes(object + static_cast<uintptr_t>(offset), bytes.data(), bytes.size()))
        {
            lua_pushnil(L);
            lua_pushstring(L, "the range cannot be read");
            return 2;
        }
        static const char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const uint8_t b : bytes)
        {
            hex.push_back(digits[b >> 4]);
            hex.push_back(digits[b & 15]);
        }
        lua_pushlstring(L, hex.data(), hex.size());
        return 1;
    }

    // native.auto_write(name, text) -> true | nil, message
    int nativeAutoWrite(lua_State* L)
    {
        size_t n = 0, len = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const char* text = luaL_checklstring(L, 2, &len);
        bool ok = false;
        {
            const std::wstring path = automationFile(name, n);
            HANDLE f = path.empty() ? INVALID_HANDLE_VALUE
                : CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(len), &written, nullptr) && written == len;
            if (f != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f);
            }
        }
        if (!ok)
        {
            return pushFailure(L, "cannot write the file");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativeAutoStartGameServer(lua_State* L)
    {
        lua_pushnumber(L, Overlay::startGameServerForTest(static_cast<int>(luaL_optnumber(L, 1, 8))));
        return 1;
    }

    // native.auto_network_flag(on) -> bool
    int nativeAutoNetworkFlag(lua_State* L)
    {
        lua_pushboolean(L, World::setNetworkFlagForTest(lua_toboolean(L, 1) != 0));
        return 1;
    }

    // native.auto_show_window(name, shown) -> bool
    int nativeAutoShowWindow(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const bool shown = lua_toboolean(L, 2) != 0;
        lua_pushboolean(L, Ui::setWindowShown(std::string(name, n), shown));
        return 1;
    }

    int nativeAutoQuit(lua_State* L)
    {
        Automation::quit(static_cast<int>(luaL_optnumber(L, 1, 0)));
    }


    void registerAutomation(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"ui_clip", nativeUiClip},
            {"ui_wheel", nativeUiWheel},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        static constexpr luaL_Reg kAutomation[] = {
            {"auto_instance", nativeAutoInstance},
            {"auto_mods", nativeAutoMods},
            {"auto_state", nativeAutoState},
            {"auto_ui", nativeAutoUi},
            {"auto_move", nativeAutoMove},
            {"auto_button", nativeAutoButton},
            {"auto_wheel", nativeAutoWheel},
            {"auto_key", nativeAutoKey},
            {"auto_char", nativeAutoChar},
            {"auto_release", nativeAutoRelease},
            {"auto_load", nativeAutoLoad},
            {"auto_screenshot", nativeAutoScreenshot},
            {"auto_object_bytes", nativeAutoObjectBytes},
            {"auto_write", nativeAutoWrite},
            {"auto_start_gameserver", nativeAutoStartGameServer},
            {"auto_network_flag", nativeAutoNetworkFlag},
            {"auto_show_window", nativeAutoShowWindow},
            {"auto_quit", nativeAutoQuit},
            {nullptr, nullptr},
        };
        if (automation)
        {
            setFunctions(L, kAutomation);
        }
    }
}
