// The Lua host's natives for saving and loading: save requests, the savegame list, pictures, pending saves.
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
    // Saving (src/gear/save.cpp).
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
        // The call first: the message is read after it (argument order is unspecified, it used to be read before).
        const bool ok = checkSlot(L, 1, slot) && Savegames::remove(slot, error);
        return pushResult(L, ok, error);
    }

    // native.save_load(slot) -> true | nil, message
    int nativeSaveLoad(lua_State* L)
    {
        uint32_t slot = 0;
        const char* error = "the slot is a whole number 0..32766";
        // The call first: the message is read after it (argument order is unspecified, it used to be read before).
        const bool ok = checkSlot(L, 1, slot) && Savegames::load(slot, error);
        return pushResult(L, ok, error);
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


    void registerSave(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"save_blocked", nativeSaveBlocked},
            {"save_request", nativeSaveRequest},
            {"saves", nativeSaves},
            {"save_free_slot", nativeSaveFreeSlot},
            {"save_reserve", nativeSaveReserve},
            {"save_picture", nativeSavePicture},
            {"save_delete", nativeSaveDelete},
            {"save_load", nativeSaveLoad},
            {"save_pending", nativeSavePending},
            {"save_info", nativeSaveInfo},
            {"save_poll", nativeSavePoll},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
