#include "lua_host.h"
#include "automation.h"
#include "gear.h"
#include "instance.h"
#include "log.h"
#include "pickup.h"
#include "pipe.h"
#include "tables.h"
#include "ui.h"
#include "world.h"

#include <windows.h>
#include <lua.hpp>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// LuaJIT on 32-bit Windows does not unwind C++ frames when a Lua error passes through them: the native functions
// below therefore raise errors (luaL_check*) only before they create C++ objects, and report failures as
// nil + message instead.

namespace
{
    HMODULE g_module = nullptr;
    std::wstring g_modsDir;     // <game>\mods
    lua_State* g_lua = nullptr;
    bool g_started = false;
    // Lua runs on whichever thread calls frame(): the engine's in-game loop, which a new game may run on a new
    // thread. Only one at a time, and never re-entered (a game function a mod calls could come back to the hook).
    std::mutex g_running;
    std::atomic<ULONGLONG> g_lastFrame{0};
    int g_kernelRef = LUA_NOREF;

    // Runaway protection: a handler that runs past its deadline is stopped with an error (count hook). LuaJIT only
    // runs hooks in the interpreter, so code the JIT compiled is not interrupted; this catches the common case of
    // an endless loop while a mod is written.
    constexpr double kLoadBudget = 5.0;     // seconds for loading all mods
    constexpr double kFrameBudget = 0.25;   // seconds for one frame's handlers
    LARGE_INTEGER g_frequency{};
    LONGLONG g_deadline = 0;
    double g_budget = 0;        // of the kernel call in progress; each handler gets all of it (native.reset_budget)

    double now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    }

    void countHook(lua_State* L, lua_Debug*)
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_deadline && t.QuadPart > g_deadline)
        {
            luaL_error(L, "time budget exceeded");
        }
    }

    void setBudget(double seconds)
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_budget = seconds;
        g_deadline = t.QuadPart + static_cast<LONGLONG>(seconds * static_cast<double>(g_frequency.QuadPart));
    }

    std::wstring widen(const char* s, size_t n)
    {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), w.data(), static_cast<int>(w.size()));
        return w;
    }

    std::string narrow(const std::wstring& w)
    {
        std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
        return s;
    }

    // A path inside a mod's folder from the mod's relative one, or empty if it would leave the folder (absolute
    // paths, drive letters, "..", device names): mods read their own files only.
    std::wstring modPath(const char* dir, size_t dirLen, const char* rel, size_t relLen)
    {
        const std::wstring d = widen(dir, dirLen);
        const std::wstring r = widen(rel, relLen);
        if (d.empty() || r.empty() || d.find_first_of(L"\\/:") != std::wstring::npos || d == L"." || d == L".." ||
            r.find(L':') != std::wstring::npos || r[0] == L'\\' || r[0] == L'/')
        {
            return {};
        }
        const std::wstring root = g_modsDir + L"\\" + d;
        const std::wstring joined = root + L"\\" + r;
        wchar_t full[MAX_PATH * 2];
        const DWORD n = GetFullPathNameW(joined.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        if (n == 0 || n >= std::size(full))
        {
            return {};
        }
        const std::wstring result(full, n);
        if (result.size() <= root.size() + 1 || _wcsnicmp(result.c_str(), root.c_str(), root.size()) != 0 ||
            result[root.size()] != L'\\')
        {
            return {};
        }
        return result;
    }

    bool readFile(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 64 * 1024 * 1024;
        if (ok)
        {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
        }
        CloseHandle(h);
        return ok;
    }

    // native.log(text)
    int nativeLog(lua_State* L)
    {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        Log::write(std::string_view(s, n));
        return 0;
    }

    // native.mod_dirs() -> {name, ...}
    int nativeModDirs(lua_State* L)
    {
        std::vector<std::string> names;
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_modsDir + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    const std::wstring name = fd.cFileName;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L".." &&
                        GetFileAttributesW((g_modsDir + L"\\" + name + L"\\mod.lua").c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        names.push_back(narrow(name));
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (size_t i = 0; i < names.size(); ++i)
        {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.read_file(dir, relative) -> text | nil, message
    int nativeReadFile(lua_State* L)
    {
        size_t dirLen = 0, relLen = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        const char* rel = luaL_checklstring(L, 2, &relLen);
        std::string text;
        const char* error = nullptr;
        {
            const std::wstring path = modPath(dir, dirLen, rel, relLen);
            if (path.empty())
            {
                error = "path outside the mod's folder";
            }
            else if (!readFile(path, text))
            {
                error = "cannot read the file";
            }
        }
        if (error)
        {
            lua_pushnil(L);
            lua_pushstring(L, error);
            return 2;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.load(text, chunkname, env) -> function | nil, message. Text only: LuaJIT bytecode is not verified and
    // could break out of the sandbox.
    int nativeLoad(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 1, &n);
        const char* name = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        if (luaL_loadbufferx(L, text, n, name, "t") != 0)
        {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        lua_pushvalue(L, 3);
        lua_setfenv(L, -2);
        return 1;
    }

    // native.time() -> seconds
    int nativeTime(lua_State* L)
    {
        lua_pushnumber(L, now());
        return 1;
    }

    // native.reset_budget(): the kernel starts each handler with the whole budget, so one slow mod does not use up
    // the time of the mods after it.
    int nativeResetBudget(lua_State*)
    {
        if (g_deadline)
        {
            setBudget(g_budget);
        }
        return 0;
    }

    // native.table_info(name) -> count, stride | nil
    int nativeTableInfo(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        Tables::Info info{};
        if (!Tables::info(std::string_view(name, n), info))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, static_cast<lua_Number>(info.count));
        lua_pushnumber(L, static_cast<lua_Number>(info.stride));
        return 2;
    }

    // native.table_read(name, index, offset, kind) -> number | nil
    int nativeTableRead(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const lua_Number index = luaL_checknumber(L, 2);
        const lua_Number offset = luaL_checknumber(L, 3);
        const char* kindName = luaL_checkstring(L, 4);
        static constexpr struct { const char* name; Tables::Kind kind; } kKinds[] = {
            {"u8", Tables::Kind::U8}, {"i8", Tables::Kind::I8}, {"u16", Tables::Kind::U16}, {"i16", Tables::Kind::I16},
            {"u32", Tables::Kind::U32}, {"i32", Tables::Kind::I32}, {"f32", Tables::Kind::F32},
        };
        const Tables::Kind* kind = nullptr;
        for (const auto& k : kKinds)
        {
            if (std::strcmp(k.name, kindName) == 0)
            {
                kind = &k.kind;
            }
        }
        double value = 0;
        if (!kind || index < 0 || offset < 0 || index > 1e9 || offset > 1e9 ||
            index != static_cast<lua_Number>(static_cast<size_t>(index)) ||
            offset != static_cast<lua_Number>(static_cast<size_t>(offset)) ||
            !Tables::read(std::string_view(name, n), static_cast<size_t>(index), static_cast<size_t>(offset), *kind, value))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, value);
        return 1;
    }

    // native.table_string(name, index, offset, max_length) -> string | nil
    int nativeTableString(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const lua_Number index = luaL_checknumber(L, 2);
        const lua_Number offset = luaL_checknumber(L, 3);
        const lua_Number maxLength = luaL_optnumber(L, 4, 64);
        if (index < 0 || offset < 0 || maxLength < 1 || index > 1e9 || offset > 1e9 || maxLength > 256 ||
            index != static_cast<lua_Number>(static_cast<size_t>(index)) ||
            offset != static_cast<lua_Number>(static_cast<size_t>(offset)))
        {
            lua_pushnil(L);
            return 1;
        }
        std::string text;
        if (!Tables::readString(std::string_view(name, n), static_cast<size_t>(index), static_cast<size_t>(offset),
                                static_cast<size_t>(maxLength), text))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // An id argument: a whole number in 1..2^31-1, or 0 if it is none.
    uint32_t checkId(lua_State* L, int arg)
    {
        const lua_Number v = luaL_checknumber(L, arg);
        return v >= 1 && v < 2147483648.0 && v == static_cast<lua_Number>(static_cast<uint32_t>(v))
                   ? static_cast<uint32_t>(v) : 0;
    }

    bool checkPosition(lua_State* L, int arg, World::Position& out)
    {
        const lua_Number x = luaL_checknumber(L, arg), y = luaL_checknumber(L, arg + 1);
        const lua_Number layer = luaL_optnumber(L, arg + 2, 0);
        if (!(x > -2e9 && x < 2e9 && y > -2e9 && y < 2e9 && layer >= 0 && layer < 256))
        {
            return false;
        }
        out = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<uint8_t>(layer)};
        return true;
    }

    int pushFailure(lua_State* L, const char* error)
    {
        lua_pushnil(L);
        lua_pushstring(L, error ? error : "failed");
        return 2;
    }

    // native.hero() -> id | nil
    int nativeHero(lua_State* L)
    {
        const uint32_t id = World::hero();
        if (!id)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.world_changes() -> whether world changes are possible (a single player game runs)
    int nativeWorldChanges(lua_State* L)
    {
        lua_pushboolean(L, World::changesAllowed());
        return 1;
    }

    // native.object(id) -> {id, type, flags, x, y, layer, creature, hp, max_hp, gold, exp, level} | nil
    int nativeObject(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        World::Object o;
        if (!id || !World::object(id, o))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 12);
        auto set = [L](const char* key, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, key); };
        set("id", o.id);
        set("type", o.type);
        set("flags", o.flags);
        set("x", o.position.x);
        set("y", o.position.y);
        set("layer", o.position.layer);
        lua_pushboolean(L, o.creature);
        lua_setfield(L, -2, "creature");
        if (o.creature)
        {
            set("hp", o.health);
            set("max_hp", o.maxHealth);
            set("gold", o.gold);
            set("exp", o.experience);
            set("level", o.level);
        }
        return 1;
    }

    // native.creatures_near(x, y, layer, radius, limit) -> {id, ...}
    int nativeCreaturesNear(lua_State* L)
    {
        World::Position at;
        const bool ok = checkPosition(L, 1, at);
        const lua_Number radius = luaL_checknumber(L, 4);
        const lua_Number limit = luaL_optnumber(L, 5, 64);
        std::vector<uint32_t> ids;
        if (ok && radius > 0 && radius < 1e6 && limit >= 1)
        {
            ids = World::creaturesNear(at, radius, static_cast<size_t>(limit < 4096 ? limit : 4096));
        }
        lua_createtable(L, static_cast<int>(ids.size()), 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            lua_pushnumber(L, ids[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.spawn(type, x, y, layer) -> id | nil, message
    int nativeSpawn(lua_State* L)
    {
        const uint32_t type = checkId(L, 1);
        World::Position at;
        if (!checkPosition(L, 2, at))
        {
            return pushFailure(L, "bad position");
        }
        const char* error = nullptr;
        const uint32_t id = type ? World::spawn(type, at, error) : 0;
        if (!id)
        {
            return pushFailure(L, type ? error : "bad type");
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.teleport(id, x, y, layer) -> true | nil, message
    int nativeTeleport(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        World::Position at;
        if (!checkPosition(L, 2, at))
        {
            return pushFailure(L, "bad position");
        }
        const char* error = "no such object";
        if (!id || !World::teleport(id, at, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_health(id, hp) / native.set_gold(id, gold) -> true | nil, message
    int nativeSetHealth(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number v = luaL_checknumber(L, 2);
        const char* error = "no such creature";
        if (!id || !(v > -1e9 && v < 1e9) || !World::setHealth(id, static_cast<int32_t>(v), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativeSetGold(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number v = luaL_checknumber(L, 2);
        const char* error = "no such hero";
        if (!id || !(v >= 0 && v < 4294967296.0) || !World::setGold(id, static_cast<uint32_t>(v), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.pipe_open(name) -> handle | nil. The kernel checks the mod's permission first.
    int nativePipeOpen(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const int handle = Pipes::open(std::string(name, n));
        if (!handle)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, handle);
        return 1;
    }

    int nativePipeClose(lua_State* L)
    {
        Pipes::close(static_cast<int>(luaL_checknumber(L, 1)));
        return 0;
    }

    // native.pipe_send(handle, text) -> bool
    int nativePipeSend(lua_State* L)
    {
        const int handle = static_cast<int>(luaL_checknumber(L, 1));
        size_t n = 0;
        const char* text = luaL_checklstring(L, 2, &n);
        lua_pushboolean(L, Pipes::send(handle, std::string(text, n)));
        return 1;
    }

    // native.pipe_poll(handle, limit) -> {{kind, text}, ...}; kind "connect", "disconnect" or "message"
    int nativePipePoll(lua_State* L)
    {
        const int handle = static_cast<int>(luaL_checknumber(L, 1));
        const lua_Number limit = luaL_optnumber(L, 2, 64);
        const std::vector<Pipes::Event> events = Pipes::poll(handle, limit >= 1 && limit < 4096 ? static_cast<size_t>(limit) : 64);
        lua_createtable(L, static_cast<int>(events.size()), 0);
        for (size_t i = 0; i < events.size(); ++i)
        {
            const Pipes::Event& e = events[i];
            lua_createtable(L, 2, 0);
            lua_pushstring(L, e.kind == Pipes::Event::Connected ? "connect"
                              : e.kind == Pipes::Event::Disconnected ? "disconnect" : "message");
            lua_rawseti(L, -2, 1);
            lua_pushlstring(L, e.text.data(), e.text.size());
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    int nativePipeConnected(lua_State* L)
    {
        lua_pushboolean(L, Pipes::connected(static_cast<int>(luaL_checknumber(L, 1))));
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

    // native.ui_image(layer, slot, id, x, y, w, h, color, fit) -> bool; fit 0 stretch, 1 contain, 2 cover
    int nativeUiImage(lua_State* L)
    {
        Ui::Command c;
        c.kind = Ui::Kind::Image;
        const uint32_t id = checkId(L, 3);
        const lua_Number fit = luaL_checknumber(L, 9);
        bool ok = uiCommon(L, c, 1) && uiBox(L, c, 4) && id && fit >= 0 && fit <= 2;
        c.image = id;
        c.color = toColor(luaL_checknumber(L, 8));
        c.fit = static_cast<Ui::Fit>(static_cast<int>(fit));
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

    // native.ui_state() -> {available, mode, exclusive, escape_menu, message_box, help, mouse_x, mouse_y,
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

    // native.key_down(vk) -> bool
    int nativeKeyDown(lua_State* L)
    {
        const lua_Number vk = luaL_checknumber(L, 1);
        lua_pushboolean(L, vk > 0 && vk < 256 && Ui::keyDown(static_cast<int>(vk)));
        return 1;
    }

    // native.write_file(dir, relative, text) -> true | nil, message. A mod's own folder only, and only files named
    // *.json, *.txt or *.lua that are not mod.lua or main.lua (a mod keeps its settings, it does not rewrite itself).
    int nativeWriteFile(lua_State* L)
    {
        size_t dirLen = 0, relLen = 0, n = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        const char* rel = luaL_checklstring(L, 2, &relLen);
        const char* text = luaL_checklstring(L, 3, &n);
        const char* error = nullptr;
        {
            const std::string r(rel, relLen);
            std::string lower = r;
            for (char& ch : lower)
            {
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            }
            auto endsWith = [&lower](const char* s) {
                const size_t k = std::strlen(s);
                return lower.size() >= k && lower.compare(lower.size() - k, k, s) == 0;
            };
            const std::wstring path = modPath(dir, dirLen, rel, relLen);
            if (path.empty() || r.find_first_of("\\/") != std::string::npos)
            {
                error = "only files directly in the mod's folder";
            }
            else if (!(endsWith(".json") || endsWith(".txt") || endsWith(".lua")) || lower == "mod.lua" || lower == "main.lua")
            {
                error = "only .json, .txt and .lua files other than mod.lua and main.lua";
            }
            else if (n > 1024 * 1024)
            {
                error = "the text is larger than 1 MB";
            }
            else
            {
                HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD written = 0;
                const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) && written == n;
                if (f != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(f);
                }
                if (!ok)
                {
                    error = "cannot write the file";
                }
            }
        }
        if (error)
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.item(id) -> {id, type, kind, flags, type_flags, on_ground, rarity, set, classes, gold, x, y, layer} | nil
    int nativeItem(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        World::Item it;
        if (!id || !World::item(id, it))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 13);
        auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
        auto flag = [L](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
        num("id", it.id);
        num("type", it.type);
        num("kind", it.kind);
        num("flags", it.flags);
        num("type_flags", it.typeFlags);
        flag("on_ground", it.onGround);
        num("rarity", it.rarity);
        flag("set", it.setItem);
        num("classes", it.classMask);
        flag("gold", it.gold);
        num("x", it.position.x);
        num("y", it.position.y);
        num("layer", it.position.layer);
        return 1;
    }

    // native.items_near(x, y, layer, radius, limit) -> {id, ...}
    int nativeItemsNear(lua_State* L)
    {
        World::Position at;
        const bool ok = checkPosition(L, 1, at);
        const lua_Number radius = luaL_checknumber(L, 4);
        const lua_Number limit = luaL_optnumber(L, 5, 64);
        std::vector<uint32_t> ids;
        if (ok && radius > 0 && radius < 1e6 && limit >= 1)
        {
            ids = World::itemsNear(at, radius, static_cast<size_t>(limit < 4096 ? limit : 4096));
        }
        lua_createtable(L, static_cast<int>(ids.size()), 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            lua_pushnumber(L, ids[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.hero_class(id) -> class (1..9) | nil, and its bit in an item's class mask
    int nativeHeroClass(lua_State* L)
    {
        const uint32_t cls = World::heroClass(checkId(L, 1));
        if (!cls)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, cls);
        lua_pushnumber(L, World::classBit(cls));
        return 2;
    }

    // native.pickup_rules({blocked ids}, {dimmed ids}, dim_alpha)
    std::vector<uint32_t> idList(lua_State* L, int arg)
    {
        std::vector<uint32_t> out;
        const size_t n = lua_objlen(L, arg);
        for (size_t i = 1; i <= n && i <= 65536; ++i)
        {
            lua_rawgeti(L, arg, static_cast<int>(i));
            const lua_Number v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (v >= 1 && v < 2147483648.0)
            {
                out.push_back(static_cast<uint32_t>(v));
            }
        }
        return out;
    }

    int nativePickupRules(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TTABLE);
        const lua_Number alpha = luaL_optnumber(L, 3, 255);
        {
            std::vector<uint32_t> blocked = idList(L, 1), dimmed = idList(L, 2);
            Pickup::setRules(std::move(blocked), std::move(dimmed),
                             static_cast<uint8_t>(alpha >= 0 && alpha <= 255 ? alpha : 255));
        }
        return 0;
    }

    int nativePickupRefused(lua_State* L)
    {
        lua_pushnumber(L, Pickup::refused());
        return 1;
    }

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

    int nativeAutoQuit(lua_State* L)
    {
        Automation::quit(static_cast<int>(luaL_optnumber(L, 1, 0)));
    }

    int atPanic(lua_State* L)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua panic: {}", msg ? msg : "(no message)");
        return 0;
    }

    // The kernel chunk, embedded with gear.rc.
    bool kernelSource(const char*& text, size_t& size)
    {
        HRSRC res = FindResourceW(g_module, MAKEINTRESOURCEW(IDR_GEAR_KERNEL), MAKEINTRESOURCEW(10));
        HGLOBAL data = res ? LoadResource(g_module, res) : nullptr;
        text = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        size = res ? SizeofResource(g_module, res) : 0;
        return text && size;
    }

    // Calls kernel[name](args on the stack) under the time budget; logs an error. Pops the arguments.
    bool callKernel(const char* name, int nargs, double budget)
    {
        lua_State* L = g_lua;
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
        lua_getfield(L, -1, name);
        lua_remove(L, -2);
        lua_insert(L, -(nargs + 1));
        setBudget(budget);
        const int status = lua_pcall(L, nargs, 0, 0);
        g_deadline = 0;
        if (status != 0)
        {
            const char* msg = lua_tostring(L, -1);
            LOG("Lua: kernel.{} failed: {}", name, msg ? msg : "(no message)");
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    void start()
    {
        g_started = true;
        QueryPerformanceFrequency(&g_frequency);
        const char* source = nullptr;
        size_t size = 0;
        if (!kernelSource(source, size))
        {
            LOG("Lua: the kernel resource is missing: mods are off");
            return;
        }
        lua_State* L = luaL_newstate();
        if (!L)
        {
            LOG("Lua: cannot create the state: mods are off");
            return;
        }
        lua_atpanic(L, atPanic);
        luaL_openlibs(L);
        lua_sethook(L, countHook, LUA_MASKCOUNT, 10000);

        if (luaL_loadbufferx(L, source, size, "=kernel", "t") != 0)
        {
            LOG("Lua: kernel does not compile: {}", lua_tostring(L, -1));
            lua_close(L);
            return;
        }
        static constexpr luaL_Reg kNative[] = {
            {"log", nativeLog},
            {"mod_dirs", nativeModDirs},
            {"read_file", nativeReadFile},
            {"load", nativeLoad},
            {"time", nativeTime},
            {"reset_budget", nativeResetBudget},
            {"table_info", nativeTableInfo},
            {"table_read", nativeTableRead},
            {"table_string", nativeTableString},
            {"hero", nativeHero},
            {"world_changes", nativeWorldChanges},
            {"object", nativeObject},
            {"creatures_near", nativeCreaturesNear},
            {"spawn", nativeSpawn},
            {"teleport", nativeTeleport},
            {"set_health", nativeSetHealth},
            {"set_gold", nativeSetGold},
            {"pipe_open", nativePipeOpen},
            {"pipe_close", nativePipeClose},
            {"pipe_send", nativePipeSend},
            {"pipe_poll", nativePipePoll},
            {"pipe_connected", nativePipeConnected},
            {"ui_rect", nativeUiRect},
            {"ui_text", nativeUiText},
            {"ui_image", nativeUiImage},
            {"ui_commit", nativeUiCommit},
            {"ui_image_count", nativeUiImageCount},
            {"ui_image_info", nativeUiImageInfo},
            {"ui_font_count", nativeUiFontCount},
            {"ui_text_size", nativeUiTextSize},
            {"ui_windows", nativeUiWindows},
            {"ui_state", nativeUiState},
            {"item", nativeItem},
            {"ui_region", nativeUiRegion},
            {"ui_move_window", nativeUiMoveWindow},
            {"ui_can_move_windows", nativeUiCanMoveWindows},
            {"layout_read", nativeLayoutRead},
            {"layout_write", nativeLayoutWrite},
            {"ui_events", nativeUiEvents},
            {"key_down", nativeKeyDown},
            {"write_file", nativeWriteFile},
            {"items_near", nativeItemsNear},
            {"hero_class", nativeHeroClass},
            {"pickup_rules", nativePickupRules},
            {"pickup_refused", nativePickupRefused},
            {nullptr, nullptr},
        };
        lua_newtable(L);
        for (const luaL_Reg* r = kNative; r->name; ++r)
        {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
        if (Automation::options().enabled)
        {
            static constexpr luaL_Reg kAutomation[] = {
                {"auto_instance", nativeAutoInstance},
                {"auto_mods", nativeAutoMods},
                {"auto_state", nativeAutoState},
                {"auto_ui", nativeAutoUi},
                {"auto_move", nativeAutoMove},
                {"auto_button", nativeAutoButton},
                {"auto_key", nativeAutoKey},
                {"auto_char", nativeAutoChar},
                {"auto_release", nativeAutoRelease},
                {"auto_load", nativeAutoLoad},
                {"auto_screenshot", nativeAutoScreenshot},
                {"auto_write", nativeAutoWrite},
                {"auto_quit", nativeAutoQuit},
                {nullptr, nullptr},
            };
            for (const luaL_Reg* r = kAutomation; r->name; ++r)
            {
                lua_pushcfunction(L, r->func);
                lua_setfield(L, -2, r->name);
            }
        }
        lua_pushstring(L, Gear::kVersion);
        lua_setfield(L, -2, "version");
        if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1))
        {
            const char* msg = lua_tostring(L, -1);
            LOG("Lua: kernel failed: {}", msg ? msg : "it did not return a table");
            lua_close(L);
            return;
        }
        g_kernelRef = luaL_ref(L, LUA_REGISTRYINDEX);
        g_lua = L;
        LOG("Lua: {} ready, mods from {}", LUAJIT_VERSION, narrow(g_modsDir));
        callKernel("load", 0, kLoadBudget);
    }
}

void LuaHost::configure(HMODULE module, const std::wstring& gameDir)
{
    g_module = module;
    g_modsDir = gameDir + L"\\mods";
}

void LuaHost::frame(float dt)
{
    g_lastFrame = GetTickCount64();
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return;
    }
    if (!g_started)
    {
        start();
    }
    if (!g_lua)
    {
        return;
    }
    lua_pushnumber(g_lua, dt);
    callKernel("tick", 1, kFrameBudget);
}

void LuaHost::idle()
{
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return;
    }
    if (!g_started)
    {
        start();
    }
    if (!g_lua)
    {
        return;
    }
    callKernel("idle", 0, kFrameBudget);
}

unsigned long long LuaHost::lastFrameTime()
{
    return g_lastFrame.load();
}
