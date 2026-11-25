#include "lua_host.h"
#include "lua_native.h"
#include "clock.h"
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

// LuaJIT on 32-bit Windows does not unwind C++ frames when a Lua error passes through them: the native functions
// below therefore raise errors (luaL_check*) only before they create C++ objects, and report failures as
// nil + message instead.

namespace LuaNative
{
    HMODULE g_module = nullptr;
    std::wstring g_modsDir;     // <game>\mods
    std::wstring g_gameDir;     // the folder of the game's exe
    lua_State* g_lua = nullptr;
    bool g_started = false;
    // Lua runs on whichever thread calls frame(): the engine's in-game loop, which a new game may run on a new
    // thread. Only one at a time, and never re-entered (a game function a mod calls could come back to the hook).
    std::mutex g_running;
    thread_local bool t_inLua = false;     // this thread is inside a kernel call (a hook it triggers must not enter Lua again)
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

    // native.list_mod_dir(dir, subfolder) -> {file name, ...} | nil, message: the files directly in a subfolder of a mod folder
    // (names only, sorted; "" is the mod folder itself)
    int nativeListModDir(lua_State* L)
    {
        size_t dirLen = 0, relLen = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        const char* rel = luaL_optlstring(L, 2, "", &relLen);
        const std::wstring path = modPath(dir, dirLen, relLen ? rel : ".", relLen ? relLen : 1);
        std::wstring folder = path;
        if (relLen == 0)
        {
            folder = g_modsDir + L"\\" + widen(dir, dirLen);
        }
        if (path.empty() && relLen != 0)
        {
            lua_pushnil(L);
            lua_pushstring(L, "path outside the mod's folder");
            return 2;
        }
        std::vector<std::string> names;
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &data);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    char utf8[MAX_PATH * 3];
                    const int n = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, utf8, sizeof(utf8), nullptr, nullptr);
                    if (n > 1)
                    {
                        names.emplace_back(utf8, static_cast<size_t>(n - 1));
                    }
                }
            } while (FindNextFileW(h, &data));
            FindClose(h);
        }
        std::sort(names.begin(), names.end());
        lua_newtable(L);
        for (size_t i = 0; i < names.size(); ++i)
        {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
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
        lua_pushnumber(L, GearClock::g_override ? GearClock::g_override() : now());
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

    // native.clipboard_set(text) -> true | nil, message: puts UTF-8 text on the Windows clipboard (as Unicode text).
    int nativeClipboardSet(lua_State* L)
    {
        size_t length = 0;
        const char* text = luaL_checklstring(L, 1, &length);
        if (length > 1024 * 1024)
        {
            lua_pushnil(L);
            lua_pushstring(L, "the text is longer than 1 MB");
            return 2;
        }
        const int wide = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wide) + 1) * sizeof(wchar_t));
        wchar_t* target = memory ? static_cast<wchar_t*>(GlobalLock(memory)) : nullptr;
        if (!target)
        {
            if (memory) GlobalFree(memory);
            lua_pushnil(L);
            lua_pushstring(L, "out of memory");
            return 2;
        }
        MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), target, wide);
        target[wide] = 0;
        GlobalUnlock(memory);
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt)
        {
            if (OpenClipboard(nullptr))
            {
                EmptyClipboard();
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;   // the clipboard owns the memory after this
                CloseClipboard();
            }
            if (!ok) Sleep(10);
        }
        if (!ok)
        {
            GlobalFree(memory);
            lua_pushnil(L);
            lua_pushstring(L, "the clipboard is not available");
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.clipboard_get() -> the Unicode text on the clipboard as UTF-8, or nil.
    int nativeClipboardGet(lua_State* L)
    {
        std::string out;
        bool have = false;
        for (int attempt = 0; attempt < 5 && !have; ++attempt)
        {
            if (OpenClipboard(nullptr))
            {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                {
                    if (const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data)))
                    {
                        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                        if (bytes > 1)
                        {
                            out.resize(static_cast<size_t>(bytes) - 1);
                            WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
                        }
                        GlobalUnlock(data);
                        have = true;
                    }
                }
                CloseClipboard();
                if (have) break;
            }
            Sleep(10);
        }
        if (!have)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    // native.game_config() -> {KEY = "value", ...}: the game's Settings.cfg (read only)
    int nativeGameConfig(lua_State* L)
    {
        // One table per reread of the file, shared by the callers (the kernel only reads it): mods ask every frame.
        static int cachedRef = LUA_NOREF;
        static uint64_t cachedGeneration = 0;
        const auto& values = GameCfg::all();
        if (cachedRef != LUA_NOREF && cachedGeneration == GameCfg::generation())
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cachedRef);
            return 1;
        }
        lua_createtable(L, 0, static_cast<int>(values.size()));
        for (const auto& [key, value] : values)
        {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, key.c_str());
        }
        if (cachedRef != LUA_NOREF)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, cachedRef);
        }
        lua_pushvalue(L, -1);
        cachedRef = luaL_ref(L, LUA_REGISTRYINDEX);
        cachedGeneration = GameCfg::generation();
        return 1;
    }

    void setFunctions(lua_State* L, const luaL_Reg* regs)
    {
        for (const luaL_Reg* r = regs; r->name; ++r)
        {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
    }

    int atPanic(lua_State* L)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua panic: {}", msg ? msg : "(no message)");
        return 0;
    }

    // A file embedded with gear.rc, by its resource name (KERNEL_LUA).
    bool resourceSource(const wchar_t* name, const char*& text, size_t& size)
    {
        HRSRC res = FindResourceW(g_module, name, MAKEINTRESOURCEW(10));
        HGLOBAL data = res ? LoadResource(g_module, res) : nullptr;
        text = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        size = res ? SizeofResource(g_module, res) : 0;
        return text && size;
    }

    bool kernelSource(const char*& text, size_t& size)
    {
        return resourceSource(L"KERNEL_LUA", text, size);
    }

    // native.builtin(name) -> the Lua source of a part of Sacred Gear that is shipped inside the DLL (the mod manager and the
    // libraries every mod gets: gear.lookup, gear.fmt, gear.toast, gear.store), or nil. `name` is a file of src/gear/lua
    // without '.lua' ('modmanager') or with another extension ('modmanager.rml'); the resource is its name in capitals with
    // '.' as '_' (gear.rc). The kernel itself is not handed out.
    int nativeBuiltin(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        std::wstring res;
        bool ext = false;
        for (size_t i = 0; i < n && n <= 64; ++i)
        {
            const char c = name[i];
            if (c >= 'a' && c <= 'z')
            {
                res += static_cast<wchar_t>(c - 'a' + 'A');
            }
            else if ((c >= '0' && c <= '9') || c == '_')
            {
                res += static_cast<wchar_t>(c);
            }
            else if (c == '.' && !ext && i > 0)
            {
                res += L'_';
                ext = true;
            }
            else
            {
                res.clear();
                break;
            }
        }
        const char* text = nullptr;
        size_t size = 0;
        if (!res.empty() && !ext)
        {
            res += L"_LUA";
        }
        if (!res.empty() && res != L"KERNEL_LUA" && resourceSource(res.c_str(), text, size))
        {
            lua_pushlstring(L, text, size);
            return 1;
        }
        lua_pushnil(L);
        return 1;
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
        t_inLua = true;
        const int status = lua_pcall(L, nargs, 0, 0);
        t_inLua = false;
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

    constexpr double kHookBudget = 0.02;    // seconds for one answer to a site hook

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
            {"clipboard_set", nativeClipboardSet},
            {"clipboard_get", nativeClipboardGet},
            {"mod_dirs", nativeModDirs},
            {"read_file", nativeReadFile},
            {"list_mod_dir", nativeListModDir},
            {"load", nativeLoad},
            {"time", nativeTime},
            {"reset_budget", nativeResetBudget},
            {"game_config", nativeGameConfig},
            {"builtin", nativeBuiltin},
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
                {nullptr, nullptr},
            };
            for (const luaL_Reg* r = kAutomation; r->name; ++r)
            {
                lua_pushcfunction(L, r->func);
                lua_setfield(L, -2, r->name);
            }
        }
        registerTables(L, Automation::options().enabled);
        registerWorld(L, Automation::options().enabled);
        registerItems(L, Automation::options().enabled);
        registerSave(L, Automation::options().enabled);
        registerUi(L, Automation::options().enabled);
        registerNet(L, Automation::options().enabled);
        registerParty(L, Automation::options().enabled);
        registerFiles(L, Automation::options().enabled);
        registerAutomation(L, Automation::options().enabled);
        RmlHost::registerNatives(L);
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

using namespace LuaNative;

std::wstring LuaHost::stateFilePath(const wchar_t* file)
{
    return statePath(file);
}

void LuaHost::configureFolders(HMODULE module, const std::wstring& gameDir, const std::wstring& modsDir)
{
    g_module = module;
    g_modsDir = modsDir;
    g_gameDir = gameDir;
    RmlHost::configure(g_modsDir);
}

void LuaHost::configure(HMODULE module, const std::wstring& gameDir)
{
    g_module = module;
    g_modsDir = gameDir + L"\\mods";
    g_gameDir = gameDir;
    RmlHost::configure(g_modsDir);
}

static ULONGLONG g_lastIdle = 0, g_lastMenuTick = 0;

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

void LuaHost::menuFrame()
{
    // The menus run no in-game frame: tick from the UI pass, unless the automation thread already ticks them.
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFrame < 250 || now - g_lastIdle < 250 || now - g_lastMenuTick < 16)
    {
        return;
    }
    g_lastMenuTick = now;
    // (not idle(): that one stamps g_lastIdle, "the automation thread ticks", and this would hold the menu's own ticks off for 250 ms after each one: 4 a second)
    tickIdle();
}

void LuaHost::idle()
{
    g_lastIdle = GetTickCount64();
    tickIdle();
}

void LuaHost::tickIdle()
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

LuaHost::Answer LuaHost::levelFactor(double level, int site, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "level_factor");
    lua_remove(L, -2);
    lua_pushnumber(L, level);
    lua_pushnumber(L, site);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 2, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.level_factor failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        const lua_Number v = lua_tonumber(L, -1);
        if (v >= 0 && v < 1e9)
        {
            out = static_cast<int>(v);
            answer = Answer::Value;
        }
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::combatConstant(const char* name, double vanilla, int index, double& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "combat_constant");
    lua_remove(L, -2);
    lua_pushstring(L, name);
    lua_pushnumber(L, vanilla);
    lua_pushnumber(L, index);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 3, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.combat_constant failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = lua_tonumber(L, -1);
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::creatureAbility(int type, int vanilla, int index, int address, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "creature_ability");
    lua_remove(L, -2);
    lua_pushnumber(L, type);
    lua_pushnumber(L, vanilla);
    lua_pushnumber(L, index);
    lua_pushnumber(L, address);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 4, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.creature_ability failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        const lua_Number v = lua_tonumber(L, -1);
        if (v >= 0 && v < 65536)
        {
            out = static_cast<int>(v);
            answer = Answer::Value;
        }
    }
    lua_pop(L, 1);
    return answer;
}


LuaHost::Answer LuaHost::abilityRoute(int type, int art, int vanilla, int action, uint32_t id, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "ability_route");
    lua_remove(L, -2);
    lua_pushnumber(L, type);
    lua_pushnumber(L, art);
    lua_pushnumber(L, vanilla);
    lua_pushnumber(L, action);
    lua_pushnumber(L, id);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 5, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.ability_route failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = static_cast<int>(lua_tonumber(L, -1));
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::runeUsable(int type, int ability, int vanilla, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "rune_usable");
    lua_remove(L, -2);
    lua_pushnumber(L, type);
    lua_pushnumber(L, ability);
    lua_pushboolean(L, vanilla);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 3, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.rune_usable failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isboolean(L, -1))
    {
        out = lua_toboolean(L, -1) ? 1 : 0;
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::pfxSelect(uint32_t fx, uint32_t creature, uint32_t ability, uint32_t level, uint32_t parent, uint32_t& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "pfx_select");
    lua_remove(L, -2);
    lua_pushnumber(L, fx);
    lua_pushnumber(L, creature);
    lua_pushnumber(L, ability);
    lua_pushnumber(L, level);
    lua_pushnumber(L, parent);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 5, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.pfx_select failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0)
    {
        out = static_cast<uint32_t>(lua_tonumber(L, -1));
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::itemUse(uint32_t item, uint32_t type, uint32_t creature, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "item_use");
    lua_remove(L, -2);
    lua_pushnumber(L, item);
    lua_pushnumber(L, type);
    lua_pushnumber(L, creature);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 3, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.item_use failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = static_cast<int>(lua_tonumber(L, -1));
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::potionPick(int mode, int flag, int tier, uintptr_t address, const char* site, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "potion_pick");
    lua_remove(L, -2);
    lua_pushnumber(L, mode);
    lua_pushnumber(L, flag);
    lua_pushnumber(L, tier);
    lua_pushnumber(L, static_cast<lua_Number>(address));
    if (site)
    {
        lua_pushstring(L, site);
    }
    else
    {
        lua_pushnil(L);
    }
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 5, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.potion_pick failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = static_cast<int>(lua_tonumber(L, -1));
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

void LuaHost::killRecorded(uint32_t id)
{
    if (t_inLua)
    {
        return;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return;
    }
    if (!g_lua || !g_started)
    {
        return;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "kill_recorded");
    lua_remove(L, -2);
    lua_pushnumber(L, id);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 0, 0);
    t_inLua = false;
    g_deadline = 0;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.kill_recorded failed: {}", msg ? msg : "(no message)");
        lua_pop(L, 1);
    }
}

void LuaHost::shopStock(const char* reason, uint32_t merchant, uint32_t container, uint32_t heroLevel)
{
    if (t_inLua)
    {
        return;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return;
    }
    if (!g_lua || !g_started)
    {
        return;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "shop_stock");
    lua_remove(L, -2);
    lua_pushstring(L, reason);
    lua_pushnumber(L, merchant);
    lua_pushnumber(L, container);
    lua_pushnumber(L, heroLevel);
    setBudget(kFrameBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 4, 0, 0);
    t_inLua = false;
    g_deadline = 0;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.shop_stock failed: {}", msg ? msg : "(no message)");
        lua_pop(L, 1);
    }
}

LuaHost::Answer LuaHost::motionSelect(int type, int slot, uint32_t id, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "motion_select");
    lua_remove(L, -2);
    lua_pushnumber(L, type);
    lua_pushnumber(L, slot);
    lua_pushnumber(L, id);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 3, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.motion_select failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = static_cast<int>(lua_tonumber(L, -1));
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::damage(int damage, int victimType, int attackerType, int index, int victimId, int attackerId, int& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "damage");
    lua_remove(L, -2);
    lua_pushnumber(L, damage);
    lua_pushnumber(L, victimType);
    lua_pushnumber(L, attackerType);
    lua_pushnumber(L, index);
    lua_pushnumber(L, victimId);
    lua_pushnumber(L, attackerId);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 6, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.damage failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        const lua_Number v = lua_tonumber(L, -1);
        if (v == v)     // not NaN; anything outside 0 .. 65534 saturates in the stub (no wrap around)
        {
            out = v >= 65535 ? 65534 : v <= 0 ? 0 : static_cast<int>(v);
            answer = Answer::Value;
        }
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::effectAdd(const EffectInfo& info, EffectAnswer& answer)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "effect_add");
    lua_remove(L, -2);
    lua_createtable(L, 0, 8);
    const auto num = [L](const char* key, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, key);
    };
    num("creature", info.creature);
    num("ability", info.ability);
    num("effect", info.effect);
    num("flags", info.flags);
    num("counter", info.counter);
    num("value", info.value);
    num("max", info.max);
    num("current", info.current);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer result = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.effect_add failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_istable(L, -1))
    {
        result = Answer::Value;
        lua_getfield(L, -1, "value");
        if (lua_isnumber(L, -1))
        {
            answer.hasValue = true;
            answer.value = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "effect");
        if (lua_isnumber(L, -1))
        {
            answer.effect = static_cast<int>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "clamp");
        if (lua_isboolean(L, -1))
        {
            answer.clamp = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "finish");
        answer.finish = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "stack");
        if (lua_isstring(L, -1))
        {
            const char* mode = lua_tostring(L, -1);
            answer.stack = std::strcmp(mode, "set") == 0 ? 1 : std::strcmp(mode, "inc") == 0 ? 2 : 0;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return result;
}

LuaHost::Answer LuaHost::dropRoll(const DropRoll& info, DropRollAnswer& answer)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "drops_roll");
    lua_remove(L, -2);
    lua_createtable(L, 0, 8);
    const auto num = [L](const char* key, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, key);
    };
    num("creature", info.creatureType);
    num("id", info.objectId);
    num("level", info.level);
    num("forced", info.forced);
    num("bonus", info.bonus);
    num("killer", info.killer);
    num("survival", info.survival);
    lua_pushboolean(L, info.network ? 1 : 0);
    lua_setfield(L, -2, "network");
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer result = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.drops_roll failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_istable(L, -1))
    {
        result = Answer::Value;
        lua_getfield(L, -1, "cancel");
        answer.cancel = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "bonus");
        if (lua_isnumber(L, -1))
        {
            const lua_Number b = lua_tonumber(L, -1);
            answer.hasBonus = true;
            answer.bonus = b <= 0 ? 0u : b >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(b);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return result;
}

LuaHost::Answer LuaHost::dropSelect(const DropSelect& info, DropSelectAnswer& answer)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "drops_select");
    lua_remove(L, -2);
    lua_createtable(L, 0, 8);
    const auto num = [L](const char* key, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, key);
    };
    num("category", info.category);
    num("level", info.level);
    num("quality", info.quality);
    num("hero_pref", info.heroPref);
    num("flags", info.flags);
    num("creature", info.creatureType);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer result = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.drops_select failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_istable(L, -1))
    {
        result = Answer::Value;
        const auto field = [L](const char* key, bool& has, uint32_t& value) {
            lua_getfield(L, -1, key);
            if (lua_isnumber(L, -1))
            {
                const lua_Number v = lua_tonumber(L, -1);
                if (v >= 0 && v < 4294967295.0)
                {
                    has = true;
                    value = static_cast<uint32_t>(v);
                }
            }
            lua_pop(L, 1);
        };
        bool hasType = false;
        field("category", answer.hasCategory, answer.category);
        field("level", answer.hasLevel, answer.level);
        field("quality", answer.hasQuality, answer.quality);
        field("type", hasType, answer.type);
    }
    lua_pop(L, 1);
    return result;
}

LuaHost::Answer LuaHost::dropRandom(double& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "drops_random");
    lua_remove(L, -2);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 0, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer result = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.drops_random failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        out = lua_tonumber(L, -1);
        result = Answer::Value;
    }
    lua_pop(L, 1);
    return result;
}

LuaHost::Answer LuaHost::bonusConstant(int index, double vanilla, int address, double& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "bonus_constant");
    lua_remove(L, -2);
    lua_pushnumber(L, index);
    lua_pushnumber(L, vanilla);
    lua_pushnumber(L, address);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 3, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.bonus_constant failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isnumber(L, -1))
    {
        const lua_Number v = lua_tonumber(L, -1);
        if (v > -1e9 && v < 1e9)
        {
            out = v;
            answer = Answer::Value;
        }
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::skillSlot(int index)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "skillslot_select");
    lua_remove(L, -2);
    lua_pushnumber(L, index);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 0, 0);
    t_inLua = false;
    g_deadline = 0;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.skillslot_select failed: {}", msg ? msg : "(no message)");
        lua_pop(L, 1);
        return Answer::None;
    }
    return Answer::Value;
}

bool LuaHost::cursorClick(bool down)
{
    if (!Gear::Addr::cWindow_handleEventInner || !Gear::Addr::g_uiEventTarget || !Gear::Addr::g_clickEventVtable || !Gear::Addr::g_cursorX ||
        !Gear::Addr::g_cursorY)
    {
        return false;
    }
    uint8_t* target = *Gear::Addr::g_uiEventTarget;
    if (!target)
    {
        return false;
    }
    // the event as ReBorn's cave sends it: the mouse event class, type 2, the cursor; the rest of the object is zero
    alignas(4) uint32_t event[0x20] = {};
    event[0] = static_cast<uint32_t>(down ? Gear::Addr::g_clickDownEventVtable.address : Gear::Addr::g_clickEventVtable.address);
    event[1] = 2;
    event[2] = static_cast<uint32_t>(*Gear::Addr::g_cursorX);
    event[3] = static_cast<uint32_t>(*Gear::Addr::g_cursorY);
    using HandleEvent = void(__thiscall*)(void* target, void* event, int flag);
    reinterpret_cast<HandleEvent>(Gear::Addr::cWindow_handleEventInner - 0x30)(target, event, 0);
    return true;
}

LuaHost::Answer LuaHost::branch(const char* name, int index, bool vanilla, int eax, bool& take)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "combat_branch");
    lua_remove(L, -2);
    lua_pushstring(L, name);
    lua_pushboolean(L, vanilla ? 1 : 0);
    lua_pushnumber(L, index);
    lua_pushnumber(L, eax);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 4, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.combat_branch failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isboolean(L, -1))
    {
        take = lua_toboolean(L, -1) != 0;
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::masterEvent(const char* event, int rune, int ability, int count, double& out)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "master_event");
    lua_remove(L, -2);
    lua_pushstring(L, event);
    lua_pushnumber(L, rune);
    lua_pushnumber(L, ability);
    lua_pushnumber(L, count);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 4, 1, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.master_event failed: {}", msg ? msg : "(no message)");
    }
    else if (lua_isboolean(L, -1))
    {
        out = lua_toboolean(L, -1) ? 1 : 0;
        answer = Answer::Value;
    }
    else if (lua_isnumber(L, -1))
    {
        out = lua_tonumber(L, -1);
        answer = Answer::Value;
    }
    lua_pop(L, 1);
    return answer;
}

LuaHost::Answer LuaHost::skillSlotKey(int character, int& slot, int& mode)
{
    if (t_inLua)
    {
        return Answer::Busy;
    }
    std::unique_lock guard(g_running, std::try_to_lock);
    if (!guard.owns_lock())
    {
        return Answer::Busy;
    }
    if (!g_lua || !g_started)
    {
        return Answer::None;
    }
    lua_State* L = g_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_kernelRef);
    lua_getfield(L, -1, "skillslot_key");
    lua_remove(L, -2);
    lua_pushnumber(L, character);
    setBudget(kHookBudget);
    t_inLua = true;
    const int status = lua_pcall(L, 1, 2, 0);
    t_inLua = false;
    g_deadline = 0;
    Answer answer = Answer::None;
    if (status != 0)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua: kernel.skillslot_key failed: {}", msg ? msg : "(no message)");
        lua_pop(L, 1);
        return Answer::None;
    }
    if (lua_isnumber(L, -2))
    {
        slot = static_cast<int>(lua_tonumber(L, -2));
        mode = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
        answer = Answer::Value;
    }
    lua_pop(L, 2);
    return answer;
}

unsigned long long LuaHost::lastFrameTime()
{
    return g_lastFrame.load();
}
