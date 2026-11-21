#include "lua_host.h"
#include "automation.h"
#include "chat.h"
#include "classes.h"
#include "itemuse.h"
#include "drops.h"
#include "gamecfg.h"
#include "gear.h"
#include "instance.h"
#include "inventory.h"
#include "itemtip.h"
#include "itemicon.h"
#include "keyblock.h"
#include "input.h"
#include "log.h"
#include "netmsg.h"
#include "pickup.h"
#include "realm.h"
#include "replace.h"
#include "rml.h"
#include "overlay.h"
#include "buffs.h"
#include "party.h"
#include "padnav.h"
#include "pipe.h"
#include "save.h"
#include "savegames.h"
#include "sitehook.h"
#include "master.h"
#include "tables.h"
#include "game/gear_addr.h"
#include "ui.h"
#include "world.h"

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

namespace
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

    // native.region_of_sector(sector) -> region record index, region byte | nil
    int nativeRegionOfSector(lua_State* L)
    {
        const lua_Number sector = luaL_checknumber(L, 1);
        size_t index = 0;
        uint8_t code = 0;
        if (!(sector >= 0 && sector < 65536) || !Tables::regionOfSector(static_cast<uint32_t>(sector), index, code))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, static_cast<lua_Number>(index));
        lua_pushnumber(L, code);
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

    // native.table_write(name, index, offset, kind, value) -> true | false
    int nativeTableWrite(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const lua_Number index = luaL_checknumber(L, 2);
        const lua_Number offset = luaL_checknumber(L, 3);
        const char* kindName = luaL_checkstring(L, 4);
        const lua_Number value = luaL_checknumber(L, 5);
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
        const bool ok = kind && index >= 0 && offset >= 0 && index < 1e9 && offset < 1e9 &&
            index == static_cast<lua_Number>(static_cast<size_t>(index)) &&
            offset == static_cast<lua_Number>(static_cast<size_t>(offset)) &&
            Tables::write(std::string_view(name, n), static_cast<size_t>(index), static_cast<size_t>(offset), *kind, value);
        lua_pushboolean(L, ok);
        return 1;
    }

    // native.text_get(id) -> string | nil: the game's text for a text id
    int nativeTextGet(lua_State* L)
    {
        const lua_Number id = luaL_checknumber(L, 1);
        std::string text;
        if (!(id >= 1 && id < 4294967296.0) || id != static_cast<lua_Number>(static_cast<uint32_t>(id)) ||
            !Tables::text(static_cast<uint32_t>(id), text))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.setting(key) -> string | nil: a value of the game's Settings.cfg ("KEY : value" lines, read each time)
    int nativeSetting(lua_State* L)
    {
        size_t n = 0;
        const char* key = luaL_checklstring(L, 1, &n);
        std::ifstream file(std::filesystem::path(g_gameDir) / L"Settings.cfg");
        std::string line;
        while (std::getline(file, line))
        {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }
            size_t end = colon;
            while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '	'))
            {
                --end;
            }
            if (end == n && line.compare(0, end, key, n) == 0)
            {
                size_t begin = colon + 1;
                while (begin < line.size() && (line[begin] == ' ' || line[begin] == '	'))
                {
                    ++begin;
                }
                size_t last = line.size();
                while (last > begin && (line[last - 1] == '\r' || line[last - 1] == ' ' || line[last - 1] == '	'))
                {
                    --last;
                }
                lua_pushlstring(L, line.data() + begin, last - begin);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    // native.overlay_info(mod id) -> file rule count, saves folder ("" if not redirected) | nil
    int nativeOverlayInfo(lua_State* L)
    {
        size_t n = 0;
        const char* id = luaL_checklstring(L, 1, &n);
        size_t files = 0;
        std::string saves;
        if (!Overlay::info(std::string(id, n), files, saves))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, static_cast<lua_Number>(files));
        lua_pushlstring(L, saves.data(), saves.size());
        return 2;
    }

    // native.overlay_resolve(path) -> the path the game's open of `path` ends up at
    int nativeOverlayResolve(lua_State* L)
    {
        size_t n = 0;
        const char* path = luaL_checklstring(L, 1, &n);
        const std::string out = Overlay::resolve(std::string(path, n));
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    // native.overlay_served(path) -> how many opens of `path` the overlay served from a mod
    int nativeOverlayServed(lua_State* L)
    {
        size_t n = 0;
        const char* path = luaL_checklstring(L, 1, &n);
        lua_pushnumber(L, static_cast<lua_Number>(Overlay::served(std::string(path, n))));
        return 1;
    }

    // native.level_factor_active(bool): whether some mod handles `level.factor`
    int nativeLevelFactorActive(lua_State* L)
    {
        SiteHook::setLevelFactorActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.site_hook_sites() -> {{hook, index, address, calls}, ...}: every site hook and how often the engine ran it
    int nativeSiteHookSites(lua_State* L)
    {
        const auto sites = SiteHook::sites();
        lua_createtable(L, static_cast<int>(sites.size()), 0);
        int n = 0;
        for (const auto& s : sites)
        {
            lua_createtable(L, 0, 4);
            lua_pushstring(L, s.hook);
            lua_setfield(L, -2, "hook");
            lua_pushnumber(L, s.index);
            lua_setfield(L, -2, "index");
            lua_pushnumber(L, static_cast<lua_Number>(s.address));
            lua_setfield(L, -2, "address");
            lua_pushnumber(L, s.calls);
            lua_setfield(L, -2, "calls");
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }

    // native.site_selftest_start() -> started; native.site_selftest_result() -> fmul, fld, registers kept | nil
    int nativeSiteSelfTestStart(lua_State* L)
    {
        lua_pushboolean(L, SiteHook::selfTestStart());
        return 1;
    }

    int nativeSiteSelfTestResult(lua_State* L)
    {
        SiteHook::SelfTest r;
        if (!SiteHook::selfTestResult(r))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, r.fmul);
        lua_pushnumber(L, r.fld);
        lua_pushboolean(L, r.registersKept);
        lua_pushnumber(L, r.abilityStore);
        lua_pushnumber(L, r.abilityAdd);
        lua_pushnumber(L, r.abilityEdx);
        lua_pushboolean(L, r.abilityKept);
        lua_pushnumber(L, r.damage);
        lua_pushnumber(L, r.effectExit);
        lua_pushnumber(L, r.effectEntry0);
        lua_pushnumber(L, r.effectEntry1);
        lua_pushnumber(L, r.effectCounter);
        lua_pushnumber(L, r.bonusInt);
        lua_pushnumber(L, r.bonusFloat);
        lua_pushnumber(L, r.slotByte);
        lua_pushnumber(L, r.branchTaken);
        lua_pushnumber(L, r.branch2Path);
        lua_pushnumber(L, r.branch2Eax);
        return 18;
    }

    // native.difficulty() -> the running game's difficulty level (0 .. 4) | nil
    int nativeDifficulty(lua_State* L)
    {
        if (!Gear::Addr::g_difficulty)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, *Gear::Addr::g_difficulty);
        return 1;
    }

    // native.master_active(event, bool): whether some mod handles `master.price`, `master.layout` or `master.exchange`
    int nativeMasterActive(lua_State* L)
    {
        Master::setActive(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }

    // native.branch_active(bool): whether some mod handles `combat.branch`
    int nativeBranchActive(lua_State* L)
    {
        SiteHook::setBranchActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.skillkey_active(bool): whether some mod handles `skillslot.key`
    int nativeSkillKeyActive(lua_State* L)
    {
        SiteHook::setSkillKeyActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.skillslot_active(bool): whether some mod handles `skillslot.select`
    int nativeSkillSlotActive(lua_State* L)
    {
        SiteHook::setSkillSlotActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.cursor_click() -> sent
    int nativeCursorClick(lua_State* L)
    {
        lua_pushboolean(L, LuaHost::cursorClick(lua_toboolean(L, 1) != 0));
        return 1;
    }

    // native.bonus_active(bool): whether some mod handles `bonus.constant`
    int nativeBonusActive(lua_State* L)
    {
        SiteHook::setBonusActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.effect_active(bool): whether some mod handles `effect.add`
    int nativeEffectActive(lua_State* L)
    {
        SiteHook::setEffectActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.damage_active(bool): whether some mod handles `damage`
    int nativeDamageActive(lua_State* L)
    {
        SiteHook::setDamageActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.creature_ability_active(bool): whether some mod handles `creature.ability`
    int nativeCreatureAbilityActive(lua_State* L)
    {
        SiteHook::setAbilityActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    uint32_t checkId(lua_State* L, int arg);
    int pushFailure(lua_State* L, const char* error);

    // native.route_active(bool) / native.motion_active(bool): whether some mod handles `ability.route` / `motion.select`
    int nativeRouteActive(lua_State* L)
    {
        Classes::setRouteActive(lua_toboolean(L, 1) != 0);
        return 0;
    }
    int nativeMotionActive(lua_State* L)
    {
        Classes::setMotionActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.creature_learn(id, ability, level) / native.creature_forget(id, ability) -> true | nil, message
    int nativeCreatureLearn(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::learn(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), static_cast<uint32_t>(luaL_optnumber(L, 3, 1)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    int nativeCreatureForget(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::forget(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.item_use_active(bool): whether some mod handles `item.use`
    int nativeItemUseActive(lua_State* L)
    {
        ItemUse::setActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.portals([mask]) -> mask | nil, message
    int nativePortals(lua_State* L)
    {
        uint32_t mask = 0, set = 0;
        const bool write = lua_isnumber(L, 1) != 0;
        if (write)
        {
            set = static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, 1)));
        }
        const char* error = "failed";
        if (!ItemUse::portals(mask, write ? &set : nullptr, error))
        {
            return pushFailure(L, error);
        }
        lua_pushnumber(L, mask);
        return 1;
    }

    // native.open_teleporter(sub) -> true | nil, message
    int nativeOpenTeleporter(lua_State* L)
    {
        const char* error = "failed";
        if (!ItemUse::openTeleporter(static_cast<int>(luaL_optinteger(L, 1, 0)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.show_text(text_id, color) -> true | nil, message
    int nativeShowText(lua_State* L)
    {
        const char* error = "failed";
        const lua_Number id = luaL_checknumber(L, 1);
        const lua_Number color = luaL_optnumber(L, 2, 0);
        if (!(id >= 1 && id < 2147483648.0) || !(color >= 0 && color < 4294967296.0) ||
            !ItemUse::showText(static_cast<uint32_t>(id), static_cast<uint32_t>(color), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.creature_effect(id, effect, value) -> true | nil, message
    int nativeCreatureEffect(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number effect = luaL_checknumber(L, 2);
        const lua_Number value = luaL_checknumber(L, 3);
        const char* error = "failed";
        void* creature = reinterpret_cast<void*>(World::objectAddress(id));
        World::Object o;
        if (!id || !creature || !World::object(id, o) || !o.creature)
        {
            return pushFailure(L, "no such creature");
        }
        if (!(effect >= 0 && effect < 256) || !(value >= -2147483648.0 && value < 2147483648.0) ||
            !ItemUse::applyEffect(creature, static_cast<uint32_t>(effect), static_cast<uint32_t>(static_cast<int32_t>(value)), error))
    // native.creature_raise(id, ability, levels) -> true | nil, message
    int nativeCreatureRaise(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::raise(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), static_cast<uint32_t>(luaL_optnumber(L, 3, 1)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.usable_active(bool): whether some mod handles `rune.usable`
    int nativeUsableActive(lua_State* L)
    {
        Classes::setUsableActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.alias_create(alias, base) -> true | nil, message; native.alias_remove(alias); native.alias_clear();
    // native.alias_list() -> {{id, base, table = 1|2, index}, ...}
    int nativeAliasCreate(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::aliasCreate(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    int nativeAliasRemove(lua_State* L)
    {
        lua_pushboolean(L, Classes::aliasRemove(static_cast<uint32_t>(luaL_checknumber(L, 1))));
        return 1;
    }
    // native.alias_link(art_alias, spell_alias | 0) -> true | nil, message; native.linked_spell(base art id) -> spell id | 0
    int nativeAliasLink(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::aliasLink(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    int nativeLinkedSpell(lua_State* L)
    {
        lua_pushinteger(L, Classes::linkedSpell(static_cast<uint32_t>(luaL_checknumber(L, 1))));
        return 1;
    }
    int nativeAliasClear(lua_State*)
    {
        Classes::aliasClear();
        return 0;
    }
    int nativeAliasList(lua_State* L)
    {
        lua_newtable(L);
        int n = 0;
        for (int which = 1; which <= 2; ++which)
        {
            uintptr_t first = 0;
            size_t count = 0;
            Classes::aliasPool(which, first, count);
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t base = 0;
                const uint32_t id = Classes::aliasId(which, i, base);
                if (!id)
                {
                    continue;
                }
                lua_newtable(L);
                lua_pushnumber(L, id);
                lua_setfield(L, -2, "id");
                lua_pushnumber(L, base);
                lua_setfield(L, -2, "base");
                lua_pushnumber(L, which);
                lua_setfield(L, -2, "table");
                lua_pushnumber(L, static_cast<lua_Number>(i));
                lua_setfield(L, -2, "index");
                lua_rawseti(L, -2, ++n);
            }
        }
        return 1;
    }

    // native.hero_record(type) -> 0x56 bytes as a string | nil; native.hero_record_set(type, offset, bytes) -> bool
    int nativeHeroRecord(lua_State* L)
    {
        uint8_t record[0x56];
        if (!Classes::heroRecord(static_cast<uint32_t>(luaL_checknumber(L, 1)), record))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, reinterpret_cast<const char*>(record), sizeof(record));
        return 1;
    }
    int nativeHeroRecordSet(lua_State* L)
    {
        size_t length = 0;
        const char* bytes = luaL_checklstring(L, 3, &length);
        lua_pushboolean(L, Classes::setHeroRecord(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)),
                                                  reinterpret_cast<const uint8_t*>(bytes), static_cast<uint32_t>(length)));
        return 1;
    }

    // native.text_add(utf8) -> id | nil, message
    int nativeTextAdd(lua_State* L)
    {
        const char* error = nullptr;
        const uint32_t id = Classes::addText(luaL_checkstring(L, 1), error);
        if (!id)
        {
            return pushFailure(L, error);
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.texture_image(name) -> image id for gear.ui.image | nil
    int nativeTextureImage(lua_State* L)
    {
        const uint32_t id = Ui::textureImage(luaL_checkstring(L, 1));
        if (!id)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.world_cursor(exclude id) -> object id (0: none), world x, y, layer, pixel x, y | nil
    int nativeWorldCursor(lua_State* L)
    {
        Classes::Cursor c;
        if (!Classes::cursor(static_cast<uint32_t>(luaL_optnumber(L, 1, 0)), c))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, c.object);
        lua_pushnumber(L, c.x);
        lua_pushnumber(L, c.y);
        lua_pushnumber(L, c.layer);
        lua_pushnumber(L, c.pixelX);
        lua_pushnumber(L, c.pixelY);
        return 6;
    }

    // native.art_in_flight(id) -> the ability id a creature object has in flight (0: none)
    int nativeArtInFlight(lua_State* L)
    {
        lua_pushnumber(L, Classes::artInFlight(checkId(L, 1)));
        return 1;
    }

    // native.text_set(id, utf8) -> true | nil, message
    int nativeTextSet(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::setText(static_cast<uint32_t>(luaL_checknumber(L, 1)), luaL_checkstring(L, 2), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.model_motion(type, slot) -> record index (0: none) | nil; native.model_set_motion(type, slot, record) -> bool;
    // native.model_has_motion(type, slot) -> bool | nil
    int nativeModelMotion(lua_State* L)
    {
        const int r = Classes::motionRecord(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)));
        if (r < 0) { lua_pushnil(L); } else { lua_pushnumber(L, r); }
        return 1;
    }
    int nativeModelSetMotion(lua_State* L)
    {
        lua_pushboolean(L, Classes::setMotionRecord(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)),
                                                    static_cast<uint32_t>(luaL_checknumber(L, 3))));
        return 1;
    }
    int nativeModelHasMotion(lua_State* L)
    {
        const int r = Classes::hasMotion(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)));
        if (r < 0) { lua_pushnil(L); } else { lua_pushboolean(L, r); }
        return 1;
    }

    // native.combat_constant_active(bool): whether some mod handles `combat.constant`
    int nativeCombatConstantActive(lua_State* L)
    {
        SiteHook::setConstantActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.site_hooks(name) -> calls, answered, busy: the counters of the site hooks of `level.factor` (default) or `combat.constant`
    int nativeSiteHooks(lua_State* L)
    {
        const char* which = luaL_optstring(L, 1, "level.factor");
        const SiteHook::Stats s = std::strcmp(which, "combat.constant") == 0 ? SiteHook::constantStats() : std::strcmp(which, "creature.ability") == 0 ? SiteHook::abilityStats() : std::strcmp(which, "damage") == 0 ? SiteHook::damageStats() : std::strcmp(which, "effect.add") == 0 ? SiteHook::effectStats() : std::strcmp(which, "bonus.constant") == 0 ? SiteHook::bonusStats() : std::strcmp(which, "skillslot.select") == 0 ? SiteHook::skillSlotStats() : std::strcmp(which, "combat.branch") == 0 ? SiteHook::branchStats() : std::strcmp(which, "skillslot.key") == 0 ? SiteHook::skillKeyStats() : SiteHook::levelFactorStats();
        lua_pushnumber(L, s.calls);
        lua_pushnumber(L, s.answered);
        lua_pushnumber(L, s.busy);
        return 3;
    }

    // native.engine_free_slot() -> the number the game's own "first free savegame number" (ENG 00619B80, replaced by
    // Savegames::install) answers, nil if it was not found (automation: the detour is called as the engine calls it).
    int nativeEngineFreeSlot(lua_State* L)
    {
        if (!Gear::Addr::cSave_firstFreeSlot)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, Gear::Addr::cSave_firstFreeSlot());
        return 1;
    }

    // native.engine_level_factor_start(level) -> started: tests of the level.factor hook (automation): a worker thread calls the
    // engine's own level function (ENG 0051CC30, whose first block is a site hook) a moment later, when Lua is not running, so the
    // answer path of the hook can be exercised; native.engine_level_factor_result() -> value | nil.
    std::atomic<int> g_levelCallState{0};       // 0 idle, 1 running, 2 done
    std::atomic<int> g_levelCallResult{0};

    int nativeEngineLevelFactorStart(lua_State* L)
    {
        const int level = static_cast<int>(luaL_checknumber(L, 1));
        const uintptr_t fn = Gear::Addr::levelFn51CC30;
        int idle = 0;
        if (!fn || !g_levelCallState.compare_exchange_strong(idle, 1))
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        std::thread([level, fn] {
            Sleep(150);
            g_levelCallResult = reinterpret_cast<int(__stdcall*)(int)>(fn)(level);
            g_levelCallState = 2;
        }).detach();
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativeEngineLevelFactorResult(lua_State* L)
    {
        if (g_levelCallState.load() != 2)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, g_levelCallResult.load());
        g_levelCallState = 0;
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

    // Saving (gear/save.cpp).
    // native.save_blocked() -> nil | why not
    int nativeSaveBlocked(lua_State* L)
    {
        const char* why = Save::blocked();
        if (why)
        {
            lua_pushstring(L, why);
        }
        else
        {
            lua_pushnil(L);
        }
        return 1;
    }

    // native.save_request(slot, name, thumbnail) -> true | nil, message
    int nativeSaveRequest(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1);
        size_t length = 0;
        const char* name = luaL_optlstring(L, 2, "", &length);
        const bool thumbnail = lua_toboolean(L, 3) != 0;
        const bool fromWindow = lua_toboolean(L, 4) != 0;
        if (slot < 0 || slot > Save::kMaxSlot || slot != static_cast<uint32_t>(slot))
        {
            lua_pushnil(L);
            lua_pushstring(L, "the slot is a whole number 0..32766");
            return 2;
        }
        const char* error = "";
        const bool ok = Save::request(static_cast<uint32_t>(slot), std::string(name, length), thumbnail, error, fromWindow);
        if (ok)
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushstring(L, error);
        return 2;
    }

    bool checkSlot(lua_State* L, int index, uint32_t& slot)
    {
        const lua_Number n = luaL_checknumber(L, index);
        if (n < 0 || n > Save::kMaxSlot || n != static_cast<uint32_t>(n))
        {
            return false;
        }
        slot = static_cast<uint32_t>(n);
        return true;
    }

    int pushResult(lua_State* L, bool ok, const char* error)
    {
        if (ok)
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushstring(L, error ? error : "failed");
        return 2;
    }

    // native.saves() -> {{slot, file, name, hero, class_id, level, version, time, mtime, size, world_date, picture}}
    int nativeSaves(lua_State* L)
    {
        const std::vector<Savegames::Info> list = Savegames::list();
        lua_createtable(L, static_cast<int>(list.size()), 0);
        int i = 0;
        for (const Savegames::Info& s : list)
        {
            lua_createtable(L, 0, 12);
            lua_pushnumber(L, s.slot);
            lua_setfield(L, -2, "slot");
            lua_pushstring(L, s.file.c_str());
            lua_setfield(L, -2, "file");
            lua_pushstring(L, s.name.c_str());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, s.hero.c_str());
            lua_setfield(L, -2, "hero");
            lua_pushnumber(L, s.classId);
            lua_setfield(L, -2, "class_id");
            lua_pushnumber(L, s.level);
            lua_setfield(L, -2, "level");
            lua_pushnumber(L, s.version);
            lua_setfield(L, -2, "version");
            lua_pushnumber(L, static_cast<lua_Number>(s.time));
            lua_setfield(L, -2, "time");
            lua_pushnumber(L, static_cast<lua_Number>(s.mtime));
            lua_setfield(L, -2, "mtime");
            lua_pushnumber(L, static_cast<lua_Number>(s.size));
            lua_setfield(L, -2, "size");
            lua_pushstring(L, s.worldDate.c_str());
            lua_setfield(L, -2, "world_date");
            lua_pushboolean(L, s.picture);
            lua_setfield(L, -2, "picture");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    // native.save_free_slot() -> the first slot from 1 whose file does not exist | nil if there is none
    int nativeSaveFreeSlot(lua_State* L)
    {
        const uint32_t slot = Savegames::firstFreeSlot();
        if (!slot)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, slot);
        return 1;
    }

    // native.save_reserve({slot, ...}): the slots free_slot and the game's own picker skip
    int nativeSaveReserve(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::vector<uint32_t> slots;
        const int n = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= n && i <= 256; ++i)
        {
            lua_rawgeti(L, 1, i);
            const lua_Number v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (v >= 1 && v <= Save::kMaxSlot && v == static_cast<uint32_t>(v))
            {
                slots.push_back(static_cast<uint32_t>(v));
            }
        }
        Savegames::setReserved(std::move(slots));
        return 0;
    }

    // native.save_picture(slot) -> picture id | nil
    int nativeSavePicture(lua_State* L)
    {
        uint32_t slot = 0;
        const uint32_t id = checkSlot(L, 1, slot) ? Savegames::picture(slot) : 0;
        if (!id)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.save_delete(slot) -> true | nil, message
    int nativeSaveDelete(lua_State* L)
    {
        uint32_t slot = 0;
        const char* error = "the slot is a whole number 0..32766";
        return pushResult(L, checkSlot(L, 1, slot) && Savegames::remove(slot, error), error);
    }

    // native.save_load(slot) -> true | nil, message
    int nativeSaveLoad(lua_State* L)
    {
        uint32_t slot = 0;
        const char* error = "the slot is a whole number 0..32766";
        return pushResult(L, checkSlot(L, 1, slot) && Savegames::load(slot, error), error);
    }

    std::string upperName(lua_State* L, int index)
    {
        std::string name = luaL_checkstring(L, index);
        for (char& c : name)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return name;
    }

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

    int nativeSavePending(lua_State* L)
    {
        lua_pushboolean(L, Save::pending());
        return 1;
    }

    void pushFileInfo(lua_State* L, const Save::FileInfo& info)
    {
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, info.exists);
        lua_setfield(L, -2, "exists");
        lua_pushnumber(L, static_cast<lua_Number>(info.size));
        lua_setfield(L, -2, "size");
        lua_pushnumber(L, static_cast<lua_Number>(info.mtime));
        lua_setfield(L, -2, "mtime");
    }

    // native.save_info(slot) -> {exists, size, mtime}
    int nativeSaveInfo(lua_State* L)
    {
        const lua_Number slot = luaL_checknumber(L, 1);
        if (slot < 0 || slot > Save::kMaxSlot || slot != static_cast<uint32_t>(slot))
        {
            lua_pushnil(L);
            return 1;
        }
        pushFileInfo(L, Save::fileInfo(static_cast<uint32_t>(slot)));
        return 1;
    }

    // native.save_poll() -> nil | {slot, ok, message, info}
    int nativeSavePoll(lua_State* L)
    {
        Save::Result r;
        if (!Save::takeResult(r))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 5);
        lua_pushboolean(L, r.external);
        lua_setfield(L, -2, "external");
        lua_pushnumber(L, r.slot);
        lua_setfield(L, -2, "slot");
        lua_pushboolean(L, r.ok);
        lua_setfield(L, -2, "ok");
        lua_pushlstring(L, r.message.data(), r.message.size());
        lua_setfield(L, -2, "message");
        pushFileInfo(L, r.info);
        lua_setfield(L, -2, "info");
        return 1;
    }

    // native.net_kill() -> nil | {victim, killer, award}: the next kill this client sent (gear/netmsg.cpp); killer
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
    // message line the game added (gear/chat.cpp).
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
        const auto& values = GameCfg::all();
        lua_createtable(L, 0, static_cast<int>(values.size()));
        for (const auto& [key, value] : values)
        {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, key.c_str());
        }
        return 1;
    }

    // native.object(id) -> {id, type, flags, x, y, layer, sector, creature, hp, max_hp, gold, exp, level, in_combat} | nil
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
        set("sector", o.position.sector);
        lua_pushboolean(L, o.creature);
        lua_setfield(L, -2, "creature");
        if (o.creature)
        {
            set("hp", o.health);
            set("max_hp", o.maxHealth);
            set("gold", o.gold);
            set("exp", o.experience);
            set("level", o.level);
            lua_pushboolean(L, o.inCombat);
            lua_setfield(L, -2, "in_combat");
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

    // native.created_active(bool): whether some mod handles `creature.created` (the engine's create is recorded only then)
    int nativeCreatedActive(lua_State* L)
    {
        World::setCreatedActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.created_suppress(bool): creations are not recorded while the `creature.created` handlers run (no event loops)
    int nativeCreatedSuppress(lua_State* L)
    {
        World::setCreatedSuppress(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.take_created() -> {{id, type, x, y, layer}, ...}, dropped: the objects the engine itself created since the last call
    // that are still live creatures (not heroes), and how many creations did not fit into the list so far.
    int nativeTakeCreated(lua_State* L)
    {
        uint64_t dropped = 0;
        const std::vector<World::Created> list = World::takeCreated(&dropped);
        lua_createtable(L, static_cast<int>(list.size()), 0);
        int n = 0;
        for (const World::Created& c : list)
        {
            World::Object o;
            if (!World::object(c.id, o) || !o.creature)
            {
                continue;
            }
            lua_createtable(L, 0, 5);
            lua_pushnumber(L, c.id);
            lua_setfield(L, -2, "id");
            lua_pushnumber(L, c.type);
            lua_setfield(L, -2, "type");
            lua_pushnumber(L, o.position.x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, o.position.y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, o.position.layer);
            lua_setfield(L, -2, "layer");
            lua_rawseti(L, -2, ++n);
        }
        lua_pushnumber(L, static_cast<lua_Number>(dropped));
        return 2;
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

    int nativeRemove(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const char* error = "no such creature";
        if (!id || !World::remove(id, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // Party (gear/party.cpp). native.party_available() -> true | false, why
    int nativePartyAvailable(lua_State* L)
    {
        const char* why = nullptr;
        const bool ok = Party::available(why);
        lua_pushboolean(L, ok);
        if (ok)
        {
            return 1;
        }
        lua_pushstring(L, why ? why : "not available");
        return 2;
    }

    // native.party_heroes() -> {id, ...}, the controlled hero first
    int nativePartyHeroes(lua_State* L)
    {
        const std::vector<uint32_t> ids = Party::heroes();
        lua_createtable(L, static_cast<int>(ids.size()), 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            lua_pushnumber(L, ids[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.party_mercenaries() -> {id, ...}
    int nativePartyMercenaries(lua_State* L)
    {
        const std::vector<uint32_t> ids = Party::mercenaries();
        lua_createtable(L, static_cast<int>(ids.size()), 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            lua_pushnumber(L, ids[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.party_recruit(class, x, y, layer) -> id | nil, message
    int nativePartyRecruit(lua_State* L)
    {
        const uint32_t heroClass = static_cast<uint32_t>(luaL_checknumber(L, 1));
        World::Position at;
        if (!checkPosition(L, 2, at))
        {
            return pushFailure(L, "bad position");
        }
        const char* error = nullptr;
        const uint32_t id = Party::recruit(heroClass, at, error);
        if (!id)
        {
            return pushFailure(L, error);
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.party_dismiss(id) / native.party_control(id) -> true | nil, message
    int nativePartyDismiss(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::dismiss(checkId(L, 1), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativePartyControl(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::control(checkId(L, 1), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_order(id, kind, target, x, y, layer, ability) -> true | nil, message; kind 'move', 'attack',
    // 'ability', 'stop'
    int nativePartyOrder(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const std::string kind = luaL_checkstring(L, 2);
        Party::Order o;
        o.kind = kind == "move" ? Party::OrderKind::Move
               : kind == "attack" ? Party::OrderKind::Attack
               : kind == "ability" ? Party::OrderKind::Ability
                                   : Party::OrderKind::Stop;
        o.target = static_cast<uint32_t>(luaL_optnumber(L, 3, 0));
        if (lua_isnumber(L, 4) && !checkPosition(L, 4, o.at))
        {
            return pushFailure(L, "bad position");
        }
        o.ability = static_cast<uint32_t>(luaL_optnumber(L, 7, 0));
        const char* error = nullptr;
        if (!Party::order(id, o, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_abilities(id) -> {{id, level, bonus, regen, regen_max}, ...} | nil
    int nativePartyAbilities(lua_State* L)
    {
        std::vector<Party::Ability> list;
        if (!Party::abilities(checkId(L, 1), list))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Party::Ability& a = list[i];
            lua_createtable(L, 0, 5);
            lua_pushnumber(L, a.id);
            lua_setfield(L, -2, "id");
            lua_pushnumber(L, a.level);
            lua_setfield(L, -2, "level");
            lua_pushnumber(L, a.bonus);
            lua_setfield(L, -2, "bonus");
            lua_pushnumber(L, a.regen);
            lua_setfield(L, -2, "regen");
            lua_pushnumber(L, a.regenMax);
            lua_setfield(L, -2, "regen_max");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.party_combos(id) -> {{id, slot, arts = {...}, levels = {...}, total, charge}, ...} | nil
    int nativePartyCombos(lua_State* L)
    {
        std::vector<Party::Combo> list;
        if (!Party::combos(checkId(L, 1), list))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Party::Combo& k = list[i];
            lua_createtable(L, 0, 6);
            lua_pushnumber(L, k.id);
            lua_setfield(L, -2, "id");
            lua_pushnumber(L, k.slot);
            lua_setfield(L, -2, "slot");
            lua_createtable(L, 4, 0);   // arts
            lua_createtable(L, 4, 0);   // levels
            int n = 0;
            for (int j = 0; j < 4; ++j)
            {
                if (!k.arts[j])
                {
                    continue;
                }
                ++n;
                lua_pushnumber(L, k.arts[j]);
                lua_rawseti(L, -3, n);
                lua_pushnumber(L, k.levels[j]);
                lua_rawseti(L, -2, n);
            }
            lua_setfield(L, -3, "levels");
            lua_setfield(L, -2, "arts");
            lua_pushnumber(L, k.total);
            lua_setfield(L, -2, "total");
            lua_pushnumber(L, k.charge);
            lua_setfield(L, -2, "charge");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.party_set_combo(id, slot, {art, ...}) -> true | nil, message
    int nativePartySetCombo(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const uint32_t slot = static_cast<uint32_t>(luaL_checknumber(L, 2));
        luaL_checktype(L, 3, LUA_TTABLE);
        std::vector<uint16_t> arts;
        const size_t n = lua_objlen(L, 3);
        for (size_t i = 1; i <= n && i <= 8; ++i)
        {
            lua_rawgeti(L, 3, static_cast<int>(i));
            arts.push_back(static_cast<uint16_t>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
        const char* error = nullptr;
        if (!Party::setCombo(id, slot, arts, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_equipment(id) -> {[slot] = item id} (slots 0..18 that hold an item) | nil
    int nativePartyEquipment(lua_State* L)
    {
        uint32_t slots[19] = {};
        if (!Party::equipment(checkId(L, 1), slots))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_newtable(L);
        for (int i = 0; i < 19; ++i)
        {
            World::Item item;
            if (slots[i] && World::item(slots[i], item))
            {
                lua_pushnumber(L, slots[i]);
                lua_rawseti(L, -2, i);
            }
        }
        return 1;
    }

    // native.object_carried(id) -> {[slot] = item id} | nil: what any live creature carries (the equipment words at +0x1A4)
    int nativeObjectCarried(lua_State* L)
    {
        uint32_t slots[19] = {};
        if (!World::carried(checkId(L, 1), slots))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_newtable(L);
        for (int i = 0; i < 19; ++i)
        {
            World::Item item;
            if (slots[i] && World::item(slots[i], item))
            {
                lua_pushnumber(L, slots[i]);
                lua_rawseti(L, -2, i);
            }
        }
        return 1;
    }

    // native.party_state(id) -> action, movement | nil
    int nativePartyState(lua_State* L)
    {
        Party::State s;
        if (!Party::state(checkId(L, 1), s))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, s.action);
        lua_pushnumber(L, s.movement);
        return 2;
    }

    // native.party_potion(id, kind) -> true | nil, message
    int nativePartyPotion(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::usePotion(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_revive(id, hp) -> true | nil, message
    int nativePartyRevive(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::revive(checkId(L, 1), static_cast<int32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_add_exp(id, amount) -> true | nil, message
    int nativePartyAddExp(lua_State* L)
    {
        const lua_Number n = luaL_checknumber(L, 2);
        if (!(n >= 1 && n <= 2e9))
        {
            return pushFailure(L, "the amount must be 1 to 2,000,000,000");
        }
        const char* error = nullptr;
        if (!Party::addExperience(checkId(L, 1), static_cast<uint32_t>(n), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_hostile(id, other) -> bool | nil
    int nativePartyHostile(lua_State* L)
    {
        const int r = Party::hostile(checkId(L, 1), checkId(L, 2));
        if (r < 0)
        {
            lua_pushnil(L);
        }
        else
        {
            lua_pushboolean(L, r);
        }
        return 1;
    }

    // Hero files. A spec names one: "save:Hero03.pax" (the game's Save folder) or "mod:heroes/x.pax" (the mod's folder).
    std::wstring heroFilePath(const char* dir, size_t dirLen, const std::string& spec)
    {
        if (spec.rfind("save:", 0) == 0)
        {
            const std::wstring name = widen(spec.data() + 5, spec.size() - 5);
            if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos || name.size() < 5 ||
                _wcsicmp(name.c_str() + name.size() - 4, L".pax") != 0)
            {
                return {};
            }
            return g_gameDir + L"\\Save\\" + name;
        }
        if (spec.rfind("mod:", 0) == 0)
        {
            const std::wstring p = modPath(dir, dirLen, spec.data() + 4, spec.size() - 4);
            return p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".pax") == 0 ? p : std::wstring{};
        }
        return {};
    }

    void pushHeroFile(lua_State* L, const std::string& spec, const Party::HeroFile& f)
    {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, spec.data(), spec.size());
        lua_setfield(L, -2, "file");
        const std::string name = narrow(f.name);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, f.heroClass);
        lua_setfield(L, -2, "class_id");
        lua_pushnumber(L, f.level);
        lua_setfield(L, -2, "level");
    }

    // native.party_hero_files(dir) -> {{file, name, class_id, level}, ...}: Save\Hero*.pax and the mod's heroes\*.pax
    int nativePartyHeroFiles(lua_State* L)
    {
        size_t dirLen = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        lua_newtable(L);
        int n = 0;
        auto scan = [&](const std::wstring& folder, const char* prefix) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((folder + L"\\*.pax").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                Party::HeroFile f;
                if (Party::readHeroFile(folder + L"\\" + fd.cFileName, f) && n < 200)
                {
                    pushHeroFile(L, std::string(prefix) + narrow(fd.cFileName), f);
                    lua_rawseti(L, -2, ++n);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        };
        scan(g_gameDir + L"\\Save", "save:");
        const std::wstring mine = modPath(dir, dirLen, "heroes", 6);
        if (!mine.empty())
        {
            scan(mine, "mod:heroes/");
        }
        return 1;
    }

    // native.party_import(dir, spec, x, y, layer) -> id | nil, message
    int nativePartyImport(lua_State* L)
    {
        size_t dirLen = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        const std::wstring path = heroFilePath(dir, dirLen, luaL_checkstring(L, 2));
        World::Position at;
        if (path.empty())
        {
            return pushFailure(L, "no such hero file");
        }
        if (!checkPosition(L, 3, at))
        {
            return pushFailure(L, "bad position");
        }
        const char* error = nullptr;
        const uint32_t id = Party::importHero(path, at, error);
        if (!id)
        {
            return pushFailure(L, error);
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.party_export(dir, name) -> spec | nil, message: the controlled hero to the mod's heroes\<name>.pax
    int nativePartyExport(lua_State* L)
    {
        size_t dirLen = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        std::string name = luaL_checkstring(L, 2);
        for (char& ch : name)
        {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            if (!ok)
            {
                ch = '_';
            }
        }
        if (name.empty() || name.size() > 48)
        {
            return pushFailure(L, "the name must be 1 to 48 characters");
        }
        const std::string rel = "heroes/" + name + ".pax";
        const std::wstring path = modPath(dir, dirLen, rel.data(), rel.size());
        if (path.empty())
        {
            return pushFailure(L, "bad file name");
        }
        const char* error = nullptr;
        if (!Party::exportHero(path, error))
        {
            return pushFailure(L, error);
        }
        const std::string spec = "mod:" + rel;
        lua_pushlstring(L, spec.data(), spec.size());
        return 1;
    }

    // native.party_set_name(id, name) -> true | nil, message
    int nativePartySetName(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::setName(checkId(L, 1), luaL_checkstring(L, 2), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_portrait(type) -> UI image index | nil
    int nativePartyPortrait(lua_State* L)
    {
        const int r = Party::portraitImage(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        if (r < 0)
        {
            lua_pushnil(L);
        }
        else
        {
            lua_pushnumber(L, r);
        }
        return 1;
    }

    // native.party_share(share, radius)
    // native.buff_apply(id, key, mods {{offset, factor, value}...}, seconds, overlay, source, ability) -> true | nil, message
    int nativeBuffApply(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        Buffs::Buff b;
        b.key = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        const int n = static_cast<int>(lua_objlen(L, 3));
        for (int i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, 3, i);
            if (lua_istable(L, -1))
            {
                Buffs::Mod m;
                lua_rawgeti(L, -1, 1);
                m.offset = static_cast<uint16_t>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                m.factor = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 3);
                m.value = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                b.mods.push_back(m);
            }
            lua_pop(L, 1);
        }
        b.seconds = luaL_optnumber(L, 4, 0);
        b.overlay = static_cast<int>(luaL_optnumber(L, 5, 0));
        b.source = static_cast<uint32_t>(luaL_optnumber(L, 6, 0));
        b.ability = static_cast<int>(luaL_optnumber(L, 7, 0));
        const char* error = "no such creature";
        if (!id || !Buffs::apply(id, b, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.buff_remove(id, key) -> bool
    int nativeBuffRemove(lua_State* L)
    {
        lua_pushboolean(L, Buffs::remove(checkId(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    // native.buff_list(id) -> {{key, seconds, left, overlay, source, ability, mods = {{offset, factor, value}...}}...}
    int nativeBuffList(lua_State* L)
    {
        const std::vector<Buffs::Buff> list = Buffs::list(checkId(L, 1));
        lua_createtable(L, static_cast<int>(list.size()), 0);
        int i = 0;
        for (const Buffs::Buff& b : list)
        {
            lua_createtable(L, 0, 7);
            lua_pushstring(L, b.key.c_str());
            lua_setfield(L, -2, "key");
            lua_pushnumber(L, b.seconds);
            lua_setfield(L, -2, "seconds");
            lua_pushnumber(L, b.left);
            lua_setfield(L, -2, "left");
            lua_pushnumber(L, b.overlay);
            lua_setfield(L, -2, "overlay");
            lua_pushnumber(L, b.source);
            lua_setfield(L, -2, "source");
            lua_pushnumber(L, b.ability);
            lua_setfield(L, -2, "ability");
            lua_createtable(L, static_cast<int>(b.mods.size()), 0);
            int j = 0;
            for (const Buffs::Mod& m : b.mods)
            {
                lua_createtable(L, 3, 0);
                lua_pushnumber(L, m.offset);
                lua_rawseti(L, -2, 1);
                lua_pushboolean(L, m.factor);
                lua_rawseti(L, -2, 2);
                lua_pushnumber(L, m.value);
                lua_rawseti(L, -2, 3);
                lua_rawseti(L, -2, ++j);
            }
            lua_setfield(L, -2, "mods");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    // native.buff_ended() -> {{id, key}...} since the last call
    int nativeBuffEnded(lua_State* L)
    {
        const auto ended = Buffs::takeEnded();
        lua_createtable(L, static_cast<int>(ended.size()), 0);
        int i = 0;
        for (const auto& [id, key] : ended)
        {
            lua_createtable(L, 2, 0);
            lua_pushnumber(L, id);
            lua_rawseti(L, -2, 1);
            lua_pushstring(L, key.c_str());
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    // native.buff_clear(): every Gear buff ends (a savegame was loaded)
    int nativeBuffClear(lua_State* L)
    {
        Buffs::clear();
        return 0;
    }

    // native.buff_fields(id) -> {[offset] = value} | nil: the stats block's float fields cStats::recalc works out
    int nativeBuffFields(lua_State* L)
    {
        std::vector<std::pair<uint16_t, float>> f;
        if (!Buffs::fields(checkId(L, 1), f))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, static_cast<int>(f.size()));
        for (const auto& [offset, value] : f)
        {
            lua_pushnumber(L, value);
            lua_rawseti(L, -2, offset);
        }
        return 1;
    }

    // native.buff_overlay(id) -> which, seconds | nil;  native.buff_set_overlay(id, which, seconds) -> bool
    int nativeBuffOverlay(lua_State* L)
    {
        int which = 0;
        float seconds = 0;
        if (!Buffs::overlay(checkId(L, 1), which, seconds))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, which);
        lua_pushnumber(L, seconds);
        return 2;
    }

    int nativeBuffSetOverlay(lua_State* L)
    {
        lua_pushboolean(L, Buffs::setOverlay(checkId(L, 1), static_cast<int>(luaL_checknumber(L, 2)),
            static_cast<float>(luaL_optnumber(L, 3, 30))));
        return 1;
    }

    // native.party_weapon_slots(id) -> active (0-based), {{main, off}...} | nil
    int nativePartyWeaponSlots(lua_State* L)
    {
        Party::WeaponSlots w;
        if (!Party::weaponSlots(checkId(L, 1), w))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, w.active);
        lua_createtable(L, 5, 0);
        for (int i = 0; i < 5; ++i)
        {
            lua_createtable(L, 2, 0);
            lua_pushnumber(L, w.main[i]);
            lua_rawseti(L, -2, 1);
            lua_pushnumber(L, w.off[i]);
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, i + 1);
        }
        return 2;
    }

    // native.party_use_weapon_slot(id, slot 0-based) -> true | nil, message
    int nativePartyUseWeaponSlot(lua_State* L)
    {
        const char* error = "not a party hero";
        if (!Party::useWeaponSlot(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.party_exp_range(id) -> experience, level start, next level start | nil
    int nativePartyExpRange(lua_State* L)
    {
        uint32_t now = 0, from = 0, to = 0;
        if (!Party::experienceRange(checkId(L, 1), now, from, to))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, now);
        lua_pushnumber(L, from);
        lua_pushnumber(L, to);
        return 3;
    }

    // native.party_shared_purse(on?) -> boolean (sets when given); native.party_give_gold(from, to, amount) -> true | nil, message
    int nativePartySharedPurse(lua_State* L)
    {
        if (lua_isboolean(L, 1))
        {
            Party::setSharedPurse(lua_toboolean(L, 1) != 0);
        }
        lua_pushboolean(L, Party::sharedPurse() ? 1 : 0);
        return 1;
    }

    int nativePartyGiveGold(lua_State* L)
    {
        const lua_Number n = luaL_checknumber(L, 3);
        const char* error = nullptr;
        if (!(n >= 0 && n < 4294967296.0) || !Party::giveGold(checkId(L, 1), checkId(L, 2), static_cast<uint32_t>(n), error))
        {
            return pushFailure(L, error ? error : "invalid amount");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativePartyShare(lua_State* L)
    {
        Party::setExperienceShare(luaL_checknumber(L, 1), luaL_optnumber(L, 2, 0));
        return 0;
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

    // native.copy_allegiance(id, from) -> true | nil, message
    int nativeCopyAllegiance(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const uint32_t from = checkId(L, 2);
        const char* error = "no such creature";
        if (!id || !from || !World::copyAllegiance(id, from, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_max_health(id, max, hp?) -> true | nil, message: a creature's maximum health, and its current health (default: the
    // same share of the new maximum as before).
    int nativeSetMaxHealth(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number v = luaL_checknumber(L, 2);
        const bool haveHp = lua_isnumber(L, 3) != 0;
        const lua_Number hp = haveHp ? lua_tonumber(L, 3) : 0;
        const char* error = "no such creature";
        if (!id || !(v >= 1 && v < 1e9) || !(hp > -1e9 && hp < 1e9) ||
            !World::setMaxHealth(id, static_cast<int32_t>(v), haveHp, static_cast<int32_t>(hp), error))
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
    std::wstring statePath(const wchar_t* file = L"gear_mods.json")
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

    // native.state_read(which?) -> text | nil. `which` 'realms' reads gear_realms.json (default: gear_mods.json).
    int nativeStateRead(lua_State* L)
    {
        std::string text;
        if (!readFile(stateFile(L, 1), text))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.state_write(text, backup) -> bool. Atomic (a temporary file moved over the old one); `backup` true
    // writes gear_mods.json.bak instead (the copy of a file the kernel could not read).
    int nativeStateWrite(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 1, &n);
        const bool backup = lua_toboolean(L, 2) != 0;
        bool ok = false;
        if (n <= 1024 * 1024)
        {
            const std::wstring path = stateFile(L, 3) + (backup ? L".bak" : L"");
            const std::wstring temp = path + L".tmp";
            HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) &&
                 written == n && FlushFileBuffers(f);
            if (f != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f);
            }
            ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok)
            {
                DeleteFileW(temp.c_str());
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

    // Atomic: written to <path>.tmp, flushed, then moved over the file, so a crash leaves the old file or the new one,
    // never half of one.
    bool writeAtomic(const std::wstring& path, const char* text, size_t n)
    {
        const std::wstring temp = path + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) &&
                  written == n && FlushFileBuffers(f);
        if (f != INVALID_HANDLE_VALUE)
        {
            CloseHandle(f);
        }
        ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
        {
            DeleteFileW(temp.c_str());
        }
        return ok;
    }

    // Per savegame data (gear.savegame): saves\<scope>\<name> of a mod's folder. A scope and a file name are made of
    // letters, digits, '_', '-' and '.'.
    bool plainName(const char* s, size_t n)
    {
        if (n == 0 || n > 96 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        {
            return false;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const char ch = s[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring scopeDir(const char* dir, size_t dirLen, const char* scope, size_t scopeLen)
    {
        if (!plainName(scope, scopeLen))
        {
            return {};
        }
        const std::string rel = "saves/" + std::string(scope, scopeLen);
        return modPath(dir, dirLen, rel.data(), rel.size());
    }

    // Every missing directory of a path inside the mods folder.
    void makeDirs(const std::wstring& path)
    {
        for (size_t i = 3; i < path.size(); ++i)
        {
            if (path[i] == L'\\')
            {
                CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
            }
        }
        CreateDirectoryW(path.c_str(), nullptr);
    }

    bool dataExtension(std::string name)
    {
        for (char& ch : name)
        {
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        }
        auto ends = [&name](const char* e) { const size_t k = std::strlen(e); return name.size() > k && name.compare(name.size() - k, k, e) == 0; };
        return ends(".json") || ends(".txt");
    }

    // native.scope_read(dir, scope, name) -> text | nil, message
    int nativeScopeRead(lua_State* L)
    {
        size_t dl = 0, sl = 0, nl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const char* name = luaL_checklstring(L, 3, &nl);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty() || !plainName(name, nl))
        {
            return pushFailure(L, "bad scope or file name");
        }
        std::string text;
        if (!readFile(root + L"\\" + widen(name, nl), text))
        {
            return pushFailure(L, "cannot read the file");
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.scope_write(dir, scope, name, text) -> true | nil, message
    int nativeScopeWrite(lua_State* L)
    {
        size_t dl = 0, sl = 0, nl = 0, n = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const char* name = luaL_checklstring(L, 3, &nl);
        const char* text = luaL_checklstring(L, 4, &n);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty() || !plainName(name, nl) || !dataExtension(std::string(name, nl)))
        {
            return pushFailure(L, "bad scope or file name (.json or .txt)");
        }
        if (n > 8 * 1024 * 1024)
        {
            return pushFailure(L, "the text is larger than 8 MB");
        }
        makeDirs(root);
        if (!writeAtomic(root + L"\\" + widen(name, nl), text, n))
        {
            return pushFailure(L, "cannot write the file");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // Deletes the files directly in `root` (not the directory).
    void clearDir(const std::wstring& root)
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                DeleteFileW((root + L"\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // native.scope_clear(dir, scope) -> true | nil, message: removes the scope's files.
    int nativeScopeClear(lua_State* L)
    {
        size_t dl = 0, sl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty())
        {
            return pushFailure(L, "bad scope name");
        }
        clearDir(root);
        RemoveDirectoryW(root.c_str());
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.scope_copy(dir, from, to) -> count | nil, message: `to` gets exactly the files of `from` (an absent `from`
    // empties it).
    int nativeScopeCopy(lua_State* L)
    {
        size_t dl = 0, fl = 0, tl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* from = luaL_checklstring(L, 2, &fl);
        const char* to = luaL_checklstring(L, 3, &tl);
        const std::wstring src = scopeDir(dir, dl, from, fl), dst = scopeDir(dir, dl, to, tl);
        if (src.empty() || dst.empty())
        {
            return pushFailure(L, "bad scope name");
        }
        if (_wcsicmp(src.c_str(), dst.c_str()) == 0)
        {
            lua_pushnumber(L, 0);
            return 1;
        }
        clearDir(dst);
        int count = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((src + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            makeDirs(dst);
            do
            {
                const std::wstring name = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (name.size() > 4 && name.compare(name.size() - 4, 4, L".tmp") == 0))
                {
                    continue;
                }
                if (CopyFileW((src + L"\\" + name).c_str(), (dst + L"\\" + name).c_str(), FALSE))
                {
                    ++count;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        else
        {
            RemoveDirectoryW(dst.c_str());
        }
        lua_pushnumber(L, count);
        return 1;
    }

    // native.scope_of(slot) / native.hero_scope_of(number) -> the scope name of a savegame / network hero file.
    int nativeScopeOf(lua_State* L)
    {
        const std::string name = Savegames::scopeOf(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    int nativeHeroScopeOf(lua_State* L)
    {
        const std::string name = Savegames::heroScopeOf(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    // native.game_command() -> kind, number, scope | nil: what the game was told to do (gear/savegames.cpp), oldest
    // first: 'load' (a savegame slot), 'hero' (a network hero file), 'new' (a new game).
    int nativeGameCommand(lua_State* L)
    {
        Savegames::Command c;
        if (!Savegames::take(c))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushstring(L, c.kind == Savegames::Command::Kind::Load ? "load"
                          : c.kind == Savegames::Command::Kind::LoadHero ? "hero" : "new");
        lua_pushnumber(L, c.number);
        lua_pushstring(L, c.scope.c_str());
        return 3;
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
            else if (n > 8 * 1024 * 1024)
            {
                error = "the text is larger than 8 MB";
            }
            else
            {
                if (!writeAtomic(path, text, n))
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

    // native.inv_can_change() -> bool, network
    int nativeInvCanChange(lua_State* L)
    {
        lua_pushboolean(L, World::inventoryChangesAllowed());
        lua_pushboolean(L, World::networkGame());
        return 2;
    }

    // native.inv_container(index) -> {columns, rows} | nil
    int nativeInvContainer(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 1);
        Inventory::Container c;
        if (!(index >= 0 && index < Inventory::kContainers) || !Inventory::container(static_cast<uint32_t>(index), c))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, c.columns);
        lua_setfield(L, -2, "columns");
        lua_pushnumber(L, c.rows);
        lua_setfield(L, -2, "rows");
        return 1;
    }

    // native.inv_items(index) -> { {id, x, y, w, h}, ... }
    int nativeInvItems(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 1);
        std::vector<Inventory::Entry> list;
        if (index >= 0 && index < Inventory::kContainers)
        {
            list = Inventory::items(static_cast<uint32_t>(index));
        }
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            lua_createtable(L, 0, 5);
            auto num = [L](const char* k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); };
            num("id", list[i].id);
            num("x", list[i].x);
            num("y", list[i].y);
            num("w", list[i].width);
            num("h", list[i].height);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // native.inv_size(item id) -> w, h | nil
    int nativeInvSize(lua_State* L)
    {
        uint32_t w = 0, h = 0;
        const uint32_t id = checkId(L, 1);
        if (!id || !Inventory::itemSize(id, w, h))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, w);
        lua_pushnumber(L, h);
        return 2;
    }

    // native.inv_find_space(index, w, h) -> x, y | nil
    int nativeInvFindSpace(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 1), w = luaL_checknumber(L, 2), h = luaL_checknumber(L, 3);
        uint32_t x = 0, y = 0;
        if (!(index >= 0 && index < Inventory::kContainers && w >= 1 && w <= 16 && h >= 1 && h <= 16) ||
            !Inventory::findSpace(static_cast<uint32_t>(index), static_cast<uint32_t>(w), static_cast<uint32_t>(h), x, y))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        return 2;
    }

    // native.inv_put(index, item id, x, y) -> true | nil, message
    int nativeInvPut(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 1), x = luaL_checknumber(L, 3), y = luaL_checknumber(L, 4);
        const uint32_t id = checkId(L, 2);
        const char* error = "bad arguments";
        if (id && index >= 0 && index < Inventory::kContainers && x >= 0 && x < 256 && y >= 0 && y < 256 &&
            Inventory::put(static_cast<uint32_t>(index), id, static_cast<uint32_t>(x), static_cast<uint32_t>(y), error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.inv_remove(index, item id) -> true | nil, message
    int nativeInvRemove(lua_State* L)
    {
        const lua_Number index = luaL_checknumber(L, 1);
        const uint32_t id = checkId(L, 2);
        const char* error = "bad arguments";
        if (id && index >= 0 && index < Inventory::kContainers &&
            Inventory::remove(static_cast<uint32_t>(index), id, error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.inv_destroy(item id) -> true | nil, message
    int nativeInvDestroy(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const char* error = "bad arguments";
        if (id && Inventory::destroy(id, error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.inv_socket(item id, upgrade id) -> true | nil, message
    int nativeInvSocket(lua_State* L)
    {
        const uint32_t id = checkId(L, 1), upgrade = checkId(L, 2);
        const char* error = "bad arguments";
        if (id && upgrade && Inventory::socket(id, upgrade, error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.item_tooltip(item id | exported bytes, x, y) -> true, width, height | nil, message
    int nativeItemTooltip(lua_State* L)
    {
        const lua_Number x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
        const char* error = "bad arguments";
        bool ok = false;
        int w = 0, h = 0;
        if (x > -4096 && x < 4096 && y > -4096 && y < 4096)
        {
            if (lua_type(L, 1) == LUA_TSTRING)
            {
                size_t n = 0;
                const char* data = lua_tolstring(L, 1, &n);
                ok = n <= 65536 && ItemTip::showExported(std::string(data, n), static_cast<int>(x), static_cast<int>(y), w, h, error);
            }
            else
            {
                const uint32_t id = checkId(L, 1);
                ok = id && ItemTip::show(id, static_cast<int>(x), static_cast<int>(y), w, h, error);
            }
        }
        if (ok)
        {
            lua_pushboolean(L, 1);
            lua_pushnumber(L, w);
            lua_pushnumber(L, h);
            return 3;
        }
        return pushFailure(L, error);
    }

    // native.item_tooltip_text(item id | exported bytes) -> text (UTF-8, with the game's colour codes) | nil, message
    int nativeItemTooltipText(lua_State* L)
    {
        const char* error = "bad arguments";
        std::wstring text;
        bool ok = false;
        if (lua_type(L, 1) == LUA_TSTRING)
        {
            size_t n = 0;
            const char* data = lua_tolstring(L, 1, &n);
            ok = n <= 65536 && ItemTip::textExported(std::string(data, n), text, error);
        }
        else
        {
            const uint32_t id = checkId(L, 1);
            ok = id && ItemTip::text(id, text, error);
        }
        if (!ok)
        {
            return pushFailure(L, error);
        }
        char utf8[16384];
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8, sizeof(utf8), nullptr, nullptr);
        lua_pushlstring(L, utf8, n > 0 ? n : 0);
        return 1;
    }

    // native.item_hover() -> item id, x, y, width, height, popup flags, canvas | nil: the item whose tooltip the game shows now
    int nativeItemHover(lua_State* L)
    {
        ItemTip::Hover h;
        if (!ItemTip::hovered(h))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, h.id);
        lua_pushnumber(L, h.x + h.frameX);
        lua_pushnumber(L, h.y + h.frameY);
        lua_pushnumber(L, h.width);
        lua_pushnumber(L, h.height);
        lua_pushnumber(L, h.flags);
        lua_pushboolean(L, !h.framed);      // x, y are canvas coordinates (add the screen's left / top edge for the layout)
        return 7;
    }

    // native.item_bonus_text(item id, entry, slot) -> text (UTF-8) | nil
    int nativeItemBonusText(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number entry = luaL_checknumber(L, 2), slot = luaL_checknumber(L, 3);
        std::wstring text;
        if (!id || entry < 0 || entry > 64 || slot < 0 || slot > 7 ||
            !ItemTip::bonusLine(id, static_cast<uint32_t>(entry), static_cast<uint32_t>(slot), text))
        {
            lua_pushnil(L);
            return 1;
        }
        char utf8[4096];
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8, sizeof(utf8), nullptr, nullptr);
        lua_pushlstring(L, utf8, n > 0 ? n : 0);
        return 1;
    }

    // native.item_equip_slot(creature id, item type) -> slot (0..18) | nil
    int nativeItemEquipSlot(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number type = luaL_checknumber(L, 2);
        const int slot = id && type >= 1 && type < 0x7E60 ? ItemTip::equipSlot(id, static_cast<uint32_t>(type)) : -1;
        if (slot < 0)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, slot);
        return 1;
    }

    // native.item_type_text(type) -> the tooltip text of a scratch item of that type (UTF-8, colour codes) | nil, message
    int nativeItemTypeText(lua_State* L)
    {
        const lua_Number v = luaL_checknumber(L, 1);
        const char* error = "bad arguments";
        std::wstring text;
        if (!(v >= 1 && v < 0x7E60) || !ItemTip::textForType(static_cast<uint32_t>(v), text, error))
        {
            return pushFailure(L, error);
        }
        char utf8[16384];
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8, sizeof(utf8), nullptr, nullptr);
        lua_pushlstring(L, utf8, n > 0 ? n : 0);
        return 1;
    }

    // native.inv_export(item id) -> bytes | nil, message
    int nativeInvExport(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const char* error = "bad arguments";
        std::string bytes;
        if (id && Inventory::exportItem(id, bytes, error))
        {
            lua_pushlstring(L, bytes.data(), bytes.size());
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.inv_import(bytes) -> item id | nil, message
    int nativeInvImport(lua_State* L)
    {
        size_t n = 0;
        const char* data = luaL_checklstring(L, 1, &n);
        const char* error = "bad arguments";
        const uint32_t id = n <= 65536 ? Inventory::importItem(std::string(data, n), error) : 0;
        if (id)
        {
            lua_pushnumber(L, id);
            return 1;
        }
        return pushFailure(L, error);
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

    // Drops (gear/drops.h). native.drops_handlers(roll, select, random); native.drops_scales(chance, find); native.drops_rng(mode, seeded, seed);
    // native.drops_rarity({[table id] = multiplier}); native.drops_rarity_table() -> {{id, weight, vanilla}}; native.drops_stats() -> table;
    // native.drops_stats_reset(); native.drops_test_roll(object id, count, bonus, forced type) -> bool; native.drops_test_progress() -> done, queued;
    // native.drops_random(n) -> 0 .. n - 1 from the fair generator.
    int nativeDropsHandlers(lua_State* L)
    {
        Drops::setHandlers(lua_toboolean(L, 1) != 0, lua_toboolean(L, 2) != 0, lua_toboolean(L, 3) != 0);
        return 0;
    }

    int nativeDropsScales(lua_State* L)
    {
        Drops::setScales(luaL_checknumber(L, 1), luaL_checknumber(L, 2));
        return 0;
    }

    int nativeDropsRng(lua_State* L)
    {
        const lua_Number seed = luaL_optnumber(L, 3, 0);
        Drops::setRng(static_cast<int>(luaL_checknumber(L, 1)), lua_toboolean(L, 2) != 0, static_cast<uint64_t>(seed < 0 ? -seed : seed));
        return 0;
    }

    int nativeDropsRarity(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        std::vector<std::pair<int, double>> multipliers;
        lua_pushnil(L);
        while (lua_next(L, 1) != 0)
        {
            if (lua_isnumber(L, -2) && lua_isnumber(L, -1))
            {
                multipliers.emplace_back(static_cast<int>(lua_tonumber(L, -2)), lua_tonumber(L, -1));
            }
            lua_pop(L, 1);
        }
        Drops::setRarityMultipliers(multipliers);
        return 0;
    }

    int nativeDropsRarityTable(lua_State* L)
    {
        const std::vector<Drops::RarityEntry> table = Drops::rarityTable();
        lua_createtable(L, static_cast<int>(table.size()), 0);
        int i = 1;
        for (const Drops::RarityEntry& e : table)
        {
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, e.id);
            lua_setfield(L, -2, "id");
            lua_pushnumber(L, e.weight);
            lua_setfield(L, -2, "weight");
            lua_pushnumber(L, e.vanilla);
            lua_setfield(L, -2, "vanilla");
            lua_rawseti(L, -2, i++);
        }
        return 1;
    }

    int nativeDropsStats(lua_State* L)
    {
        const Drops::Stats s = Drops::stats();
        lua_createtable(L, 0, 12);
        const auto num = [L](const char* key, double v) {
            lua_pushnumber(L, v);
            lua_setfield(L, -2, key);
        };
        num("rolls", static_cast<double>(s.rolls));
        num("forced", static_cast<double>(s.forcedRolls));
        num("vetoed", static_cast<double>(s.vetoed));
        num("picks", static_cast<double>(s.picks));
        num("overridden", static_cast<double>(s.overridden));
        num("rarity_draws", static_cast<double>(s.rarityDraws));
        num("rand_calls", static_cast<double>(s.randCalls));
        num("lua_draws", static_cast<double>(s.luaDraws));
        lua_createtable(L, 16, 0);
        for (int q = 0; q < 16; ++q)
        {
            lua_pushnumber(L, static_cast<double>(s.quality[q]));
            lua_rawseti(L, -2, q + 1);          // index quality + 1
        }
        lua_setfield(L, -2, "quality");
        lua_createtable(L, 32, 0);
        for (int c = 0; c < 32; ++c)
        {
            lua_pushnumber(L, static_cast<double>(s.category[c]));
            lua_rawseti(L, -2, c + 1);          // index category + 1
        }
        lua_setfield(L, -2, "category");
        return 1;
    }

    int nativeDropsStatsReset(lua_State*)
    {
        Drops::resetStats();
        return 0;
    }

    int nativeDropsTestRoll(lua_State* L)
    {
        lua_pushboolean(L, Drops::testRoll(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<int>(luaL_checknumber(L, 2)),
                                           static_cast<uint32_t>(luaL_optnumber(L, 3, 0)), static_cast<uint32_t>(luaL_optnumber(L, 4, 0))));
        return 1;
    }

    int nativeDropsTestProgress(lua_State* L)
    {
        const auto p = Drops::testProgress();
        lua_pushnumber(L, p.first);
        lua_pushnumber(L, p.second);
        return 2;
    }

    int nativeDropsRandom(lua_State* L)
    {
        const lua_Number n = luaL_checknumber(L, 1);
        lua_pushnumber(L, Drops::randomBelow(static_cast<uint32_t>(n < 1 ? 1 : n)));
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

    int atPanic(lua_State* L)
    {
        const char* msg = lua_tostring(L, -1);
        LOG("Lua panic: {}", msg ? msg : "(no message)");
        return 0;
    }

    // The kernel chunk, embedded with gear.rc.
    bool resourceSource(int id, const char*& text, size_t& size)
    {
        HRSRC res = FindResourceW(g_module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
        HGLOBAL data = res ? LoadResource(g_module, res) : nullptr;
        text = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        size = res ? SizeofResource(g_module, res) : 0;
        return text && size;
    }

    bool kernelSource(const char*& text, size_t& size)
    {
        return resourceSource(IDR_GEAR_KERNEL, text, size);
    }

    // native.builtin(name) -> the Lua source of a part of Sacred Gear that is shipped inside the DLL (the mod manager and the
    // libraries every mod gets: gear.lookup, gear.fmt, gear.toast, gear.store), or nil.
    int nativeBuiltin(lua_State* L)
    {
        static const struct
        {
            const char* name;
            int id;
        } builtins[] = {
            {"modmanager", IDR_GEAR_MODMANAGER},
            {"lookup", IDR_GEAR_LOOKUP},
            {"fmt", IDR_GEAR_FMT},
            {"toast", IDR_GEAR_TOAST},
            {"store", IDR_GEAR_STORE},
            {"modmanager.rml", IDR_GEAR_MODMANAGER_UI},
        };
        const char* name = luaL_checkstring(L, 1);
        const char* text = nullptr;
        size_t size = 0;
        for (const auto& b : builtins)
        {
            if (std::strcmp(name, b.name) == 0 && resourceSource(b.id, text, size))
            {
                lua_pushlstring(L, text, size);
                return 1;
            }
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
            {"table_info", nativeTableInfo},
            {"region_of_sector", nativeRegionOfSector},
            {"table_read", nativeTableRead},
            {"table_string", nativeTableString},
            {"table_write", nativeTableWrite},
            {"text_get", nativeTextGet},
            {"setting", nativeSetting},
            {"overlay_info", nativeOverlayInfo},
            {"overlay_resolve", nativeOverlayResolve},
            {"overlay_served", nativeOverlayServed},
            {"level_factor_active", nativeLevelFactorActive},
            {"site_hooks", nativeSiteHooks},
            {"combat_constant_active", nativeCombatConstantActive},
            {"difficulty", nativeDifficulty},
            {"creature_ability_active", nativeCreatureAbilityActive},
            {"route_active", nativeRouteActive},
            {"usable_active", nativeUsableActive},
            {"item_use_active", nativeItemUseActive},
            {"open_teleporter", nativeOpenTeleporter},
            {"show_text", nativeShowText},
            {"creature_effect", nativeCreatureEffect},
            {"portals", nativePortals},
            {"alias_create", nativeAliasCreate},
            {"alias_remove", nativeAliasRemove},
            {"alias_clear", nativeAliasClear},
            {"alias_link", nativeAliasLink},
            {"linked_spell", nativeLinkedSpell},
            {"alias_list", nativeAliasList},
            {"hero_record", nativeHeroRecord},
            {"hero_record_set", nativeHeroRecordSet},
            {"text_set", nativeTextSet},
            {"text_add", nativeTextAdd},
            {"art_in_flight", nativeArtInFlight},
            {"world_cursor", nativeWorldCursor},
            {"texture_image", nativeTextureImage},
            {"motion_active", nativeMotionActive},
            {"creature_learn", nativeCreatureLearn},
            {"creature_forget", nativeCreatureForget},
            {"model_motion", nativeModelMotion},
            {"model_set_motion", nativeModelSetMotion},
            {"model_has_motion", nativeModelHasMotion},
            {"damage_active", nativeDamageActive},
            {"effect_active", nativeEffectActive},
            {"bonus_active", nativeBonusActive},
            {"skillslot_active", nativeSkillSlotActive},
            {"skillkey_active", nativeSkillKeyActive},
            {"branch_active", nativeBranchActive},
            {"master_active", nativeMasterActive},
            {"creature_raise", nativeCreatureRaise},
            {"cursor_click", nativeCursorClick},
            {"site_hook_sites", nativeSiteHookSites},
            {"site_selftest_start", nativeSiteSelfTestStart},
            {"site_selftest_result", nativeSiteSelfTestResult},
            {"engine_level_factor_start", nativeEngineLevelFactorStart},
            {"engine_level_factor_result", nativeEngineLevelFactorResult},
            {"engine_free_slot", nativeEngineFreeSlot},
            {"hero", nativeHero},
            {"world_changes", nativeWorldChanges},
            {"save_blocked", nativeSaveBlocked},
            {"save_request", nativeSaveRequest},
            {"save_pending", nativeSavePending},
            {"save_info", nativeSaveInfo},
            {"save_free_slot", nativeSaveFreeSlot},
            {"save_reserve", nativeSaveReserve},
            {"save_poll", nativeSavePoll},
            {"saves", nativeSaves},
            {"save_picture", nativeSavePicture},
            {"save_delete", nativeSaveDelete},
            {"save_load", nativeSaveLoad},
            {"ui_replace_known", nativeUiReplaceKnown},
            {"ui_replace_claims", nativeUiReplaceClaims},
            {"ui_replace_beat", nativeUiReplaceBeat},
            {"ui_window_open", nativeUiWindowOpen},
            {"ui_close_window", nativeUiCloseWindow},
            {"ui_menu_entries", nativeUiMenuEntries},
            {"ui_menu_click", nativeUiMenuClick},
            {"game_config", nativeGameConfig},
            {"net_kill", nativeNetKill},
            {"created_active", nativeCreatedActive},
            {"take_created", nativeTakeCreated},
            {"created_suppress", nativeCreatedSuppress},
            {"mp_send_frame", nativeMpSendFrame},
            {"mp_take_frame", nativeMpTakeFrame},
            {"net_ops", nativeNetOps},
            {"net_configure", nativeNetConfigure},
            {"net_send_op", nativeNetSendOp},
            {"net_take_op", nativeNetTakeOp},
            {"mp_info", nativeMpInfo},
            {"mp_mode", nativeMpMode},
            {"hero_saved", nativeHeroSaved},
            {"game_command", nativeGameCommand},
            {"scope_of", nativeScopeOf},
            {"hero_scope_of", nativeHeroScopeOf},
            {"scope_read", nativeScopeRead},
            {"scope_write", nativeScopeWrite},
            {"scope_copy", nativeScopeCopy},
            {"scope_clear", nativeScopeClear},
            {"chat_take", nativeChatTake},
            {"chat_hide", nativeChatHide},
            {"chat_info", nativeChatInfo},
            {"item_color", nativeItemColor},
            {"hero_name", nativeHeroName},
            {"object", nativeObject},
            {"creatures_near", nativeCreaturesNear},
            {"spawn", nativeSpawn},
            {"teleport", nativeTeleport},
            {"remove", nativeRemove},
            {"set_health", nativeSetHealth},
            {"copy_allegiance", nativeCopyAllegiance},
            {"set_max_health", nativeSetMaxHealth},
            {"set_gold", nativeSetGold},
            {"party_available", nativePartyAvailable},
            {"party_heroes", nativePartyHeroes},
            {"party_mercenaries", nativePartyMercenaries},
            {"party_recruit", nativePartyRecruit},
            {"party_dismiss", nativePartyDismiss},
            {"party_control", nativePartyControl},
            {"party_order", nativePartyOrder},
            {"party_abilities", nativePartyAbilities},
            {"party_state", nativePartyState},
            {"party_combos", nativePartyCombos},
            {"party_equipment", nativePartyEquipment},
            {"object_carried", nativeObjectCarried},
            {"party_set_combo", nativePartySetCombo},
            {"party_add_exp", nativePartyAddExp},
            {"party_revive", nativePartyRevive},
            {"party_potion", nativePartyPotion},
            {"party_hostile", nativePartyHostile},
            {"party_portrait", nativePartyPortrait},
            {"party_set_name", nativePartySetName},
            {"party_hero_files", nativePartyHeroFiles},
            {"party_import", nativePartyImport},
            {"party_export", nativePartyExport},
            {"party_share", nativePartyShare},
            {"party_shared_purse", nativePartySharedPurse},
            {"party_give_gold", nativePartyGiveGold},
            {"party_exp_range", nativePartyExpRange},
            {"party_weapon_slots", nativePartyWeaponSlots},
            {"party_use_weapon_slot", nativePartyUseWeaponSlot},
            {"buff_apply", nativeBuffApply},
            {"buff_remove", nativeBuffRemove},
            {"buff_list", nativeBuffList},
            {"buff_ended", nativeBuffEnded},
            {"buff_clear", nativeBuffClear},
            {"buff_fields", nativeBuffFields},
            {"buff_overlay", nativeBuffOverlay},
            {"buff_set_overlay", nativeBuffSetOverlay},
            {"pipe_open", nativePipeOpen},
            {"pipe_close", nativePipeClose},
            {"pipe_send", nativePipeSend},
            {"pipe_poll", nativePipePoll},
            {"pipe_connected", nativePipeConnected},
            {"ui_rect", nativeUiRect},
            {"ui_text", nativeUiText},
            {"ui_image", nativeUiImage},
            {"ui_item_icon", nativeUiItemIcon},
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
            {"state_read", nativeStateRead},
            {"state_write", nativeStateWrite},
            {"builtin", nativeBuiltin},
            {"ui_events", nativeUiEvents},
            {"ui_nav", nativeUiNav},
            {"ui_nav_native", nativeUiNavNative},
            {"ui_pad_info", nativeUiPadInfo},
            {"ui_pad", nativeUiPad},
            {"ui_page", nativeUiPage},
            {"ui_select", nativeUiSelect},
            {"ui_clip", nativeUiClip},
            {"ui_wheel", nativeUiWheel},
            {"key_down", nativeKeyDown},
            {"input_rules", nativeInputRules},
            {"input_events", nativeInputEvents},
            {"input_context", nativeInputContext},
            {"ui_block_keys", nativeUiBlockKeys},
            {"ui_take_typed", nativeUiTakeTyped},
            {"write_file", nativeWriteFile},
            {"items_near", nativeItemsNear},
            {"hero_class", nativeHeroClass},
            {"pickup_rules", nativePickupRules},
            {"pickup_refused", nativePickupRefused},
    {"drops_handlers", nativeDropsHandlers},
    {"drops_scales", nativeDropsScales},
    {"drops_rng", nativeDropsRng},
    {"drops_rarity", nativeDropsRarity},
    {"drops_rarity_table", nativeDropsRarityTable},
    {"drops_stats", nativeDropsStats},
    {"drops_stats_reset", nativeDropsStatsReset},
    {"drops_test_roll", nativeDropsTestRoll},
    {"drops_test_progress", nativeDropsTestProgress},
    {"drops_random", nativeDropsRandom},
            {"inv_can_change", nativeInvCanChange},
            {"inv_container", nativeInvContainer},
            {"inv_items", nativeInvItems},
            {"inv_size", nativeInvSize},
            {"inv_find_space", nativeInvFindSpace},
            {"inv_put", nativeInvPut},
            {"inv_remove", nativeInvRemove},
            {"inv_destroy", nativeInvDestroy},
            {"inv_socket", nativeInvSocket},
            {"item_tooltip", nativeItemTooltip},
            {"item_tooltip_text", nativeItemTooltipText},
            {"item_type_text", nativeItemTypeText},
            {"item_hover", nativeItemHover},
            {"item_equip_slot", nativeItemEquipSlot},
            {"item_bonus_text", nativeItemBonusText},
            {"inv_export", nativeInvExport},
            {"inv_import", nativeInvImport},
            {"realm_get", nativeRealmGet},
            {"realm_select", nativeRealmSelect},
            {"realm_valid_host", nativeRealmValidHost},
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
                {"auto_wheel", nativeAutoWheel},
                {"auto_key", nativeAutoKey},
                {"auto_pad", nativeAutoPad},
                {"auto_page", nativeAutoPage},
                {"auto_scroll", nativeAutoScroll},
                {"auto_nav", nativeAutoNav},
                {"auto_char", nativeAutoChar},
                {"auto_release", nativeAutoRelease},
                {"auto_load", nativeAutoLoad},
                {"auto_screenshot", nativeAutoScreenshot},
                {"auto_write", nativeAutoWrite},
                {"auto_object_bytes", nativeAutoObjectBytes},
                {"auto_show_window", nativeAutoShowWindow},
                {"auto_network_flag", nativeAutoNetworkFlag},
                {"auto_network_mode", nativeAutoNetworkMode},
                {"auto_hero_from_server", nativeAutoHeroFromServer},
                {"auto_chat_add", nativeAutoChatAdd},
                {"auto_chat_send", nativeAutoChatSend},
                {"auto_quit", nativeAutoQuit},
                {"auto_start_gameserver", nativeAutoStartGameServer},
                {nullptr, nullptr},
            };
            for (const luaL_Reg* r = kAutomation; r->name; ++r)
            {
                lua_pushcfunction(L, r->func);
                lua_setfield(L, -2, r->name);
            }
        }
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

std::wstring LuaHost::stateFilePath(const wchar_t* file)
{
    return statePath(file);
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
