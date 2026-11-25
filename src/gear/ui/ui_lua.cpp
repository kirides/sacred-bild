// The Lua host's natives for mod UI: replaced windows, drawing, text, the layout file, regions, the pad, input and keys.
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
    // native.ui_replace_known(name) -> bool
    int nativeUiReplaceKnown(lua_State* L)
    {
        lua_pushboolean(L, Replace::known(upperName(L, 1)));
        return 1;
    }

    // native.ui_replace_claims({names}): the windows the mods replace, once per tick
    int nativeUiReplaceClaims(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::vector<std::string> names;
        const int n = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= n && i <= 64; ++i)
        {
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) == LUA_TSTRING)
            {
                names.emplace_back(lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
        Replace::setClaims(names);
        return 0;
    }

    // native.ui_replace_beat(name) -> bool: the replacement is drawn; whether the window is open and hidden
    int nativeUiReplaceBeat(lua_State* L)
    {
        lua_pushboolean(L, Replace::beat(upperName(L, 1)));
        return 1;
    }

    // native.ui_window_open(name) -> bool (in-game windows and the menus' screens)
    int nativeUiWindowOpen(lua_State* L)
    {
        lua_pushboolean(L, Replace::open(upperName(L, 1)));
        return 1;
    }

    // native.ui_close_window(name) -> bool
    int nativeUiCloseWindow(lua_State* L)
    {
        lua_pushboolean(L, Replace::close(upperName(L, 1)));
        return 1;
    }

    // native.ui_menu_entries(name) -> {{text, x, y, w, h}, ...}: the entries of the game menu (UI_WND_ESCMENU), in layout units
    int nativeUiMenuEntries(lua_State* L)
    {
        const std::vector<Replace::Entry> list = Replace::entries(upperName(L, 1));
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Replace::Entry& e = list[i];
            lua_createtable(L, 0, 6);
            const std::string text = narrow(e.text);
            lua_pushlstring(L, text.data(), text.size());
            lua_setfield(L, -2, "text");
            lua_pushnumber(L, e.textId);
            lua_setfield(L, -2, "text_id");
            lua_pushnumber(L, e.x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, e.y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, e.w);
            lua_setfield(L, -2, "w");
            lua_pushnumber(L, e.h);
            lua_setfield(L, -2, "h");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.ui_menu_click(name, index) -> bool: presses the index-th (1-based) entry as the game's own click would
    int nativeUiMenuClick(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 2);
        lua_pushboolean(L, index >= 1 && index < 64 && Replace::clickEntry(upperName(L, 1), static_cast<size_t>(index) - 1));
        return 1;
    }

    // UI drawing (ui.h). Arguments are checked by the kernel; here only ranges.
    uint32_t toColor(lua_Number v)
    {
        return v >= 0 && v < 4294967296.0 ? static_cast<uint32_t>(static_cast<int64_t>(v)) : 0xFFFFFFFFu;
    }

    bool uiCommon(lua_State* L, Ui::Command& c, int layerArg)
    {
        const lua_Number layer = luaL_checknumber(L, layerArg);
        const lua_Number slot = luaL_checknumber(L, layerArg + 1);
        if (!(layer >= 0 && layer < static_cast<int>(Ui::Layer::Count)) || !(slot >= -1 && slot < 23))
        {
            return false;
        }
        c.layer = static_cast<Ui::Layer>(static_cast<int>(layer));
        c.window = static_cast<int8_t>(slot);
        return true;
    }

    bool uiBox(lua_State* L, Ui::Command& c, int arg)
    {
        const lua_Number v[4] = {luaL_checknumber(L, arg), luaL_checknumber(L, arg + 1), luaL_optnumber(L, arg + 2, 0),
                                 luaL_optnumber(L, arg + 3, 0)};
        for (lua_Number n : v)
        {
            if (!(n > -100000 && n < 100000))
            {
                return false;
            }
        }
        c.x = static_cast<float>(v[0]);
        c.y = static_cast<float>(v[1]);
        c.w = static_cast<float>(v[2]);
        c.h = static_cast<float>(v[3]);
        return true;
    }

    // native.ui_rect(layer, slot, x, y, w, h, color) -> bool
    int nativeUiRect(lua_State* L)
    {
        Ui::Command c;
        c.kind = Ui::Kind::Rect;
        const bool ok = uiCommon(L, c, 1) && uiBox(L, c, 3);
        c.color = toColor(luaL_checknumber(L, 7));
        lua_pushboolean(L, ok && Ui::add(std::move(c)));
        return 1;
    }

    // A vector font's arguments at `index`: face (string or nil: the default), size (layout units), style flags.
    bool textStyle(lua_State* L, int index, Text::Style& out)
    {
        size_t n = 0;
        const char* face = lua_isstring(L, index) ? lua_tolstring(L, index, &n) : nullptr;
        const lua_Number size = luaL_optnumber(L, index + 1, 13), flags = luaL_optnumber(L, index + 2, 0);
        if (n > 64 || !(size >= 2 && size <= 200) || flags < 0 || flags > 255)
        {
            return false;
        }
        out.face = face ? widen(face, n) : std::wstring();
        out.size = static_cast<float>(size);
        out.flags = static_cast<uint32_t>(flags);
        return true;
    }

    // native.ui_text(layer, slot, text, x, y, w, h, color, font, flags, background, padding) -> bool
    int nativeUiText(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 3, &n);
        const lua_Number font = luaL_checknumber(L, 9), flags = luaL_checknumber(L, 10);
        const lua_Number color = luaL_checknumber(L, 8), background = luaL_checknumber(L, 11);
        const lua_Number padding = luaL_checknumber(L, 12);
        bool ok = false;
        {
            Ui::Command c;
            c.kind = Ui::Kind::Text;
            // font -1: a vector font (13 face or nil, 14 size, 15 style flags); 16 valign (0 top, 1 middle, 2 bottom,
            // 3 baseline)
            c.vector = font == -1;
            const lua_Number valign = luaL_optnumber(L, 16, 0);
            c.valign = valign >= 0 && valign <= 3 ? static_cast<uint8_t>(valign) : 0;
            ok = n <= 4096 && uiCommon(L, c, 1) && uiBox(L, c, 4) && (c.vector || (font >= 0 && font < 256)) &&
                 flags >= 0 && flags < 4294967296.0 && padding >= 0 && padding < 1000 &&
                 (!c.vector || textStyle(L, 13, c.style));
            if (ok)
            {
                c.text = widen(text, n);
                c.color = toColor(color);
                c.font = c.vector ? 0 : static_cast<uint16_t>(font);
                c.flags = static_cast<uint32_t>(static_cast<int64_t>(flags));
                c.background = toColor(background);
                c.padding = static_cast<float>(padding);
                ok = Ui::add(std::move(c));
            }
        }
        lua_pushboolean(L, ok);
        return 1;
    }

    // native.ui_image(layer, slot, id, x, y, w, h, color, fit) -> bool; fit 0 stretch, 1 contain, 2 cover, plus 4 for a
    // left-right mirrored and 8 for an upside-down image
    int nativeUiImage(lua_State* L)
    {
        Ui::Command c;
        c.kind = Ui::Kind::Image;
        const uint32_t id = checkId(L, 3);
        const lua_Number mode = luaL_checknumber(L, 9);
        const int fit = static_cast<int>(mode) & 3;
        bool ok = uiCommon(L, c, 1) && uiBox(L, c, 4) && id && mode >= 0 && mode < 16 && fit <= 2;
        c.image = id;
        c.color = toColor(luaL_checknumber(L, 8));
        c.fit = static_cast<Ui::Fit>(fit);
        c.flip = static_cast<uint8_t>((static_cast<int>(mode) >> 2) & 3);
        for (int i = 0; i < 4; ++i)
        {
            c.crop[i] = static_cast<float>(luaL_optnumber(L, 10 + i, i < 2 ? 0.0 : 1.0));
        }
        lua_pushboolean(L, ok && Ui::add(std::move(c)));
        return 1;
    }

    // native.ui_item_icon(layer, slot, x, y, w, h, item id (0: none), type) -> bool
    int nativeUiItemIcon(lua_State* L)
    {
        Ui::Command c;
        c.kind = Ui::Kind::ItemIcon;
        const lua_Number item = luaL_checknumber(L, 7), type = luaL_checknumber(L, 8);
        bool ok = uiCommon(L, c, 1) && uiBox(L, c, 3) && item >= 0 && item < 4294967296.0 && type >= 0 && type < 65536 &&
                  (item > 0 || type > 0);
        c.region = ok ? static_cast<uint32_t>(item) : 0;
        c.image = ok ? static_cast<uint32_t>(type) : 0;
        lua_pushboolean(L, ok && Ui::add(std::move(c)));
        return 1;
    }

    int nativeUiCommit(lua_State*)
    {
        Ui::commit();
        return 0;
    }

    // native.ui_image_count() -> n (ids 1..n-1); native.ui_image_info(id) -> {sheet, width, height, loaded} | nil
    int nativeUiImageCount(lua_State* L)
    {
        lua_pushnumber(L, Ui::imageCount());
        return 1;
    }

    int nativeUiImageInfo(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        Ui::ImageInfo info;
        if (!id || !Ui::imageInfo(id, info))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, info.sheet.data(), info.sheet.size());
        lua_setfield(L, -2, "sheet");
        lua_pushnumber(L, info.width);
        lua_setfield(L, -2, "width");
        lua_pushnumber(L, info.height);
        lua_setfield(L, -2, "height");
        lua_pushboolean(L, info.loaded);
        lua_setfield(L, -2, "loaded");
        return 1;
    }

    int nativeUiFontCount(lua_State* L)
    {
        lua_pushnumber(L, Ui::fontCount());
        return 1;
    }

    // native.ui_text_size(text, font) -> width, height | nil
    int nativeUiTextSize(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 1, &n);
        const lua_Number font = luaL_checknumber(L, 2);
        if (font == -1)
        {
            // A vector font (3 face, 4 size, 5 style flags): layout units at the canvas's scale.
            Text::Style style;
            float fw = 0, fh = 0;
            if (n > 4096 || !textStyle(L, 3, style) || !Ui::textSize(style, widen(text, n), fw, fh))
            {
                lua_pushnil(L);
                return 1;
            }
            lua_pushnumber(L, fw);
            lua_pushnumber(L, fh);
            return 2;
        }
        int w = 0, h = 0;
        bool ok = false;
        if (n <= 4096 && font >= 0 && font < 256)
        {
            const std::wstring t = widen(text, n);
            ok = Ui::textSize(static_cast<uint16_t>(font), t, w, h);
        }
        if (!ok)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, w);
        lua_pushnumber(L, h);
        return 2;
    }

    void pushWindow(lua_State* L, const Ui::WindowInfo& w)
    {
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, w.name.data(), w.name.size());
        lua_setfield(L, -2, "name");
        auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
        num("slot", w.slot);
        num("x", w.x);
        num("y", w.y);
        num("width", w.width);
        num("height", w.height);
        lua_pushboolean(L, w.visible);
        lua_setfield(L, -2, "visible");
        lua_pushboolean(L, w.drawn);
        lua_setfield(L, -2, "drawn");
        num("frame_x", w.frameX);
        num("frame_y", w.frameY);
        lua_pushboolean(L, w.movable);
        lua_setfield(L, -2, "movable");
        if (w.hasBounds)
        {
            lua_createtable(L, 0, 4);
            num("x", w.boundsX);
            num("y", w.boundsY);
            num("w", w.boundsW);
            num("h", w.boundsH);
            lua_setfield(L, -2, "bounds");
        }
    }

    // native.ui_text_focus() -> x, y, w, h | nil: the game's text field that has the keyboard focus (layout units).
    int nativeUiTextFocus(lua_State* L)
    {
        float x, y, w, h;
        if (!Ui::focusedTextField(x, y, w, h))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushnumber(L, w);
        lua_pushnumber(L, h);
        return 4;
    }

    // native.ui_type_text(utf8) -> n: types the text into the game's focused text field (see Ui::typeText).
    int nativeUiTypeText(lua_State* L)
    {
        size_t length = 0;
        const char* text = luaL_checklstring(L, 1, &length);
        std::wstring wide;
        if (length > 0)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
            if (n > 0)
            {
                wide.resize(static_cast<size_t>(n));
                MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), wide.data(), n);
            }
        }
        lua_pushinteger(L, Ui::typeText(wide));
        return 1;
    }

    // native.ui_windows() -> {window, ...}
    int nativeUiWindows(lua_State* L)
    {
        const std::vector<Ui::WindowInfo> list = Ui::windows();
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            pushWindow(L, list[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.ui_state() -> {available, mode, exclusive, escape_menu, message_box, help, show_text, mouse_x, mouse_y,
    // screen_left, screen_top, screen_right, screen_bottom}
    int nativeUiState(lua_State* L)
    {
        const Ui::State s = Ui::state();
        lua_createtable(L, 0, 12);
        auto flag = [L](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
        auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
        flag("available", s.available);
        lua_pushstring(L, s.mode);
        lua_setfield(L, -2, "mode");
        if (!s.exclusive.empty())
        {
            lua_pushlstring(L, s.exclusive.data(), s.exclusive.size());
            lua_setfield(L, -2, "exclusive");
        }
        flag("escape_menu", s.escapeMenu);
        flag("message_box", s.messageBox);
        flag("help", s.help);
        flag("show_text", s.showText);
        num("mouse_x", s.mouseX);
        num("mouse_y", s.mouseY);
        num("screen_left", s.screenLeft);
        num("screen_top", s.screenTop);
        num("screen_right", s.screenRight);
        num("screen_bottom", s.screenBottom);
        return 1;
    }

    // native.ui_move_window(slot, x, y, set) -> bool; native.ui_can_move_windows() -> bool
    int nativeUiMoveWindow(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1), x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
        const bool set = lua_toboolean(L, 4) != 0;
        lua_pushboolean(L, slot >= 0 && slot < 23 && x > -100000 && x < 100000 && y > -100000 && y < 100000 &&
                               Ui::moveWindow(static_cast<int>(slot), static_cast<float>(x), static_cast<float>(y), set));
        return 1;
    }

    int nativeUiCanMoveWindows(lua_State* L)
    {
        lua_pushboolean(L, Ui::canMoveWindows());
        return 1;
    }

    // The layout file: <game>\mods\gear_layout.json, the positions of the mods' panels and the game's windows.
    std::wstring layoutPath() { return g_modsDir + L"\\gear_layout.json"; }

    int nativeLayoutRead(lua_State* L)
    {
        std::string text;
        bool ok = false;
        {
            ok = readFile(layoutPath(), text);
        }
        if (!ok)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    int nativeLayoutWrite(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 1, &n);
        bool ok = false;
        if (n <= 1024 * 1024)
        {
            const std::wstring path = layoutPath();
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) && written == n;
            if (f != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f);
            }
        }
        lua_pushboolean(L, ok);
        return 1;
    }

    // mods\gear_mods.json: the mod manager's file (enabled flags, settings, key binds of all mods). An automation run
    // with an instance name keeps its own copy in GearAutomation\<instance>, so tests never touch the player's file.
    std::wstring statePath(const wchar_t* file)
    {
        if (Automation::options().enabled)
        {
            const std::wstring dir = Instance::directory(g_modsDir.substr(0, g_modsDir.size() - 5));
            if (!dir.empty())
            {
                return dir + L"\\" + file;
            }
        }
        return g_modsDir + L"\\" + file;
    }

    // The file a state call means: its argument 'realms' is mods\gear_realms.json.
    std::wstring stateFile(lua_State* L, int arg)
    {
        const char* name = lua_isstring(L, arg) ? lua_tostring(L, arg) : nullptr;
        return statePath(name && std::strcmp(name, "realms") == 0 ? L"gear_realms.json" : L"gear_mods.json");
    }

    // native.ui_region(layer, x, y, w, h, handle) -> bool
    int nativeUiRegion(lua_State* L)
    {
        Ui::Command c;
        c.kind = Ui::Kind::Region;
        const lua_Number layer = luaL_checknumber(L, 1);
        const lua_Number handle = luaL_checknumber(L, 6);
        bool ok = layer >= 0 && layer < static_cast<int>(Ui::Layer::Count) && uiBox(L, c, 2) && handle >= 1 &&
                  handle < 4294967296.0;
        c.layer = static_cast<Ui::Layer>(ok ? static_cast<int>(layer) : 0);
        c.region = ok ? static_cast<uint32_t>(handle) : 0;
        lua_pushboolean(L, ok && Ui::add(std::move(c)));
        return 1;
    }

    // native.ui_nav(stops, flags) -> bool: tells SacredBild the widgets the controller can point at. `stops`: a list of
    // {x, y, left, top, width, height, kind}; flags: 1 a modal screen (only these are stops), 2 a window is open.
    int nativeUiNav(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        const int flags = static_cast<int>(luaL_checknumber(L, 2));
        std::vector<PadNav::Stop> stops;
        const int n = static_cast<int>(lua_objlen(L, 1));
        stops.reserve(static_cast<size_t>(n));
        const auto field = [&](const char* name) {
            lua_getfield(L, -1, name);
            const float v = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            return v;
        };
        for (int i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, 1, i);
            if (lua_istable(L, -1))
            {
                stops.push_back({field("x"), field("y"), field("left"), field("top"), field("width"),
                                 field("height"), static_cast<int>(field("kind"))});
            }
            lua_pop(L, 1);
        }
        lua_pushboolean(L, PadNav::setStops(stops, flags));
        return 1;
    }

    // native.ui_nav_native(rects): the stops (a list of {left, top, width, height}) that draw their own button prompt: SacredBild draws none beside them.
    // Nothing happens when SacredBild has no such export.
    int nativeUiNavNative(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::vector<float> rects;
        const int n = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, 1, i);
            if (lua_istable(L, -1))
            {
                for (const char* name : {"left", "top", "width", "height"})
                {
                    lua_getfield(L, -1, name);
                    rects.push_back(static_cast<float>(lua_tonumber(L, -1)));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        PadNav::setNativePrompts(rects);
        return 0;
    }

    // native.ui_pad() -> nudge, back, menu: what the controller did since the last call (slider steps, B presses,
    // mod menu button presses).
    int nativeUiPad(lua_State* L)
    {
        const PadNav::Events e = PadNav::take();
        lua_pushnumber(L, e.nudge);
        lua_pushnumber(L, e.back);
        lua_pushnumber(L, e.menu);
        return 3;
    }

    // native.ui_pad_info() -> controller, style: whether the controller was used last, and the pad's style (0 Xbox, 1 PlayStation,
    // 2 Switch).
    int nativeUiPadInfo(lua_State* L)
    {
        int style = 0;
        const bool controller = PadNav::padInfo(style);
        lua_pushboolean(L, controller);
        lua_pushnumber(L, style);
        return 2;
    }

    // native.auto_pad(nudge, back, menu): automation posts pad events; native.auto_nav() -> stops, flags as SacredBild
    // holds them (stops -1: stale, -2: no SacredBild).
    int nativeAutoPad(lua_State* L)
    {
        PadNav::post(static_cast<int>(luaL_optnumber(L, 1, 0)), static_cast<int>(luaL_optnumber(L, 2, 0)),
                     static_cast<int>(luaL_optnumber(L, 3, 0)));
        return 0;
    }

    // native.ui_page() -> pages: LB / RB presses since the last call (+ next page, - previous).
    int nativeUiPage(lua_State* L)
    {
        lua_pushnumber(L, PadNav::takePage());
        return 1;
    }

    // native.ui_select() -> presses of the pad's Select / "-" button since the last call.
    int nativeUiSelect(lua_State* L)
    {
        lua_pushnumber(L, PadNav::takeSelect());
        return 1;
    }

    int nativeAutoScroll(lua_State* L)
    {
        PadNav::postScroll(static_cast<int>(luaL_checknumber(L, 1)));
        return 0;
    }

    int nativeAutoPage(lua_State* L)
    {
        PadNav::postPage(static_cast<int>(luaL_checknumber(L, 1)));
        return 0;
    }

    int nativeAutoNav(lua_State* L)
    {
        int flags = 0;
        const int count = PadNav::navState(flags);
        lua_pushnumber(L, count);
        lua_pushnumber(L, flags);
        return 2;
    }

    // native.ui_events() -> {{down, button, x, y, region}, ...}
    int nativeUiEvents(lua_State* L)
    {
        const std::vector<Ui::InputEvent> events = Ui::takeEvents();
        lua_createtable(L, static_cast<int>(events.size()), 0);
        for (size_t i = 0; i < events.size(); ++i)
        {
            const Ui::InputEvent& e = events[i];
            lua_createtable(L, 0, 5);
            lua_pushboolean(L, e.down);
            lua_setfield(L, -2, "down");
            lua_pushnumber(L, e.button);
            lua_setfield(L, -2, "button");
            lua_pushnumber(L, e.x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, e.y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, e.region);
            lua_setfield(L, -2, "region");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.input_rules(rules): {{id, from, to, ctrl, shift, alt, any}} (VK_ codes) for gear.input
    int nativeInputRules(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::vector<Input::Rule> rules;
        const int n = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, 1, i);
            if (lua_istable(L, -1))
            {
                Input::Rule r;
                lua_getfield(L, -1, "id");
                if (lua_isstring(L, -1))
                {
                    r.id = lua_tostring(L, -1);
                }
                lua_pop(L, 1);
                lua_getfield(L, -1, "from");
                r.from = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "to");
                r.to = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "ctrl");
                r.ctrl = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_getfield(L, -1, "shift");
                r.shift = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_getfield(L, -1, "alt");
                r.alt = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_getfield(L, -1, "any");
                r.anyModifiers = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_getfield(L, -1, "poll");
                r.poll = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                if (r.from > 0 && r.from < 256)
                {
                    rules.push_back(std::move(r));
                }
            }
            lua_pop(L, 1);
        }
        Input::setRules(std::move(rules));
        return 0;
    }

    // native.input_events() -> {{id, down, repeat}}
    int nativeInputEvents(lua_State* L)
    {
        const std::vector<Input::Event> events = Input::takeEvents();
        lua_createtable(L, static_cast<int>(events.size()), 0);
        int i = 1;
        for (const Input::Event& e : events)
        {
            lua_createtable(L, 0, 3);
            lua_pushstring(L, e.id.c_str());
            lua_setfield(L, -2, "id");
            lua_pushboolean(L, e.down);
            lua_setfield(L, -2, "down");
            lua_pushboolean(L, e.repeat);
            lua_setfield(L, -2, "repeat");
            lua_rawseti(L, -2, i++);
        }
        return 1;
    }

    // native.input_context() -> remapping applies now, typing
    int nativeInputContext(lua_State* L)
    {
        lua_pushboolean(L, Input::context());
        lua_pushboolean(L, Input::typing());
        return 2;
    }

    // native.ui_block_keys(blocked): the keyboard stays from the game while true (called every tick by the kernel).
    int nativeUiBlockKeys(lua_State* L)
    {
        KeyBlock::set(lua_toboolean(L, 1) != 0);
        KeyBlock::tick();
        return 0;
    }

    // native.ui_take_typed() -> string (UTF-8): what the keys kept from the game typed since the last call
    int nativeUiTakeTyped(lua_State* L)
    {
        const std::wstring typed = KeyBlock::takeTyped();
        std::string out;
        if (!typed.empty())
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), nullptr, 0, nullptr, nullptr);
            out.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), out.data(), n, nullptr, nullptr);
        }
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    // native.key_down(vk) -> bool
    int nativeKeyDown(lua_State* L)
    {
        const lua_Number vk = luaL_checknumber(L, 1);
        lua_pushboolean(L, vk > 0 && vk < 256 && Ui::keyDown(static_cast<int>(vk)));
        return 1;
    }


    void registerUi(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"ui_replace_known", nativeUiReplaceKnown},
            {"ui_replace_claims", nativeUiReplaceClaims},
            {"ui_replace_beat", nativeUiReplaceBeat},
            {"ui_window_open", nativeUiWindowOpen},
            {"ui_close_window", nativeUiCloseWindow},
            {"ui_menu_entries", nativeUiMenuEntries},
            {"ui_menu_click", nativeUiMenuClick},
            {"ui_rect", nativeUiRect},
            {"ui_text", nativeUiText},
            {"ui_image", nativeUiImage},
            {"ui_item_icon", nativeUiItemIcon},
            {"ui_commit", nativeUiCommit},
            {"ui_image_count", nativeUiImageCount},
            {"ui_image_info", nativeUiImageInfo},
            {"ui_font_count", nativeUiFontCount},
            {"ui_text_size", nativeUiTextSize},
            {"ui_text_focus", nativeUiTextFocus},
            {"ui_type_text", nativeUiTypeText},
            {"ui_windows", nativeUiWindows},
            {"ui_state", nativeUiState},
            {"ui_move_window", nativeUiMoveWindow},
            {"ui_can_move_windows", nativeUiCanMoveWindows},
            {"layout_read", nativeLayoutRead},
            {"layout_write", nativeLayoutWrite},
            {"ui_region", nativeUiRegion},
            {"ui_nav", nativeUiNav},
            {"ui_nav_native", nativeUiNavNative},
            {"ui_pad", nativeUiPad},
            {"ui_pad_info", nativeUiPadInfo},
            {"ui_page", nativeUiPage},
            {"ui_select", nativeUiSelect},
            {"ui_events", nativeUiEvents},
            {"input_rules", nativeInputRules},
            {"input_events", nativeInputEvents},
            {"input_context", nativeInputContext},
            {"ui_block_keys", nativeUiBlockKeys},
            {"ui_take_typed", nativeUiTakeTyped},
            {"key_down", nativeKeyDown},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        static constexpr luaL_Reg kAutomation[] = {
            {"auto_pad", nativeAutoPad},
            {"auto_scroll", nativeAutoScroll},
            {"auto_page", nativeAutoPage},
            {"auto_nav", nativeAutoNav},
            {nullptr, nullptr},
        };
        if (automation)
        {
            setFunctions(L, kAutomation);
        }
    }
}
