// The Lua host's natives for the engine's data tables, game text and settings, the file overlay, site hooks and map markers.
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

    // native.map_markers() -> {{x, y, kind, region}, ...} | nil: the service symbols of the world map (bin\merc.bin, 16-byte records
    // region, tile x, tile y, kind; kind 0 merchant, 1 blacksmith, 2 combo master, 3 horse trader). Read once, then kept.
    int nativeMapMarkers(lua_State* L)
    {
        static std::vector<int32_t> records;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            std::ifstream file(std::filesystem::path(g_gameDir) / L"bin" / L"merc.bin", std::ios::binary);
            if (file)
            {
                std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                size_t count = bytes.size() / 16;
                records.resize(count * 4);
                if (count) memcpy(records.data(), bytes.data(), count * 16);
            }
        }
        if (records.empty())
        {
            lua_pushnil(L);
            return 1;
        }
        size_t count = records.size() / 4;
        lua_createtable(L, (int)count, 0);
        for (size_t i = 0; i < count; ++i)
        {
            const int32_t* r = &records[i * 4];
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, r[1]);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, r[2]);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, r[3]);
            lua_setfield(L, -2, "kind");
            lua_pushnumber(L, r[0]);
            lua_setfield(L, -2, "region");
            lua_rawseti(L, -2, (int)i + 1);
        }
        return 1;
    }


    void registerTables(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
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
            {"site_hook_sites", nativeSiteHookSites},
            {"site_selftest_start", nativeSiteSelfTestStart},
            {"site_selftest_result", nativeSiteSelfTestResult},
            {"difficulty", nativeDifficulty},
            {"map_markers", nativeMapMarkers},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
