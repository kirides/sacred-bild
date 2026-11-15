#include "lua_host.h"
#include "gear.h"
#include "log.h"
#include "tables.h"

#include <windows.h>
#include <lua.hpp>

#include <cstring>
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
    DWORD g_thread = 0;
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
        g_thread = GetCurrentThreadId();
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
            {nullptr, nullptr},
        };
        lua_newtable(L);
        for (const luaL_Reg* r = kNative; r->name; ++r)
        {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
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
    if (!g_started)
    {
        start();
    }
    if (!g_lua || GetCurrentThreadId() != g_thread)
    {
        return;
    }
    lua_pushstring(g_lua, "frame");
    lua_pushnumber(g_lua, dt);
    callKernel("dispatch", 2, kFrameBudget);
}
