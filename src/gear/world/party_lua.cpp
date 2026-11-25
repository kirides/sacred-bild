// The Lua host's natives for the party (companion heroes, hero files) and buffs.
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
    // Party (src/gear/world/party.cpp). native.party_available() -> true | false, why
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

    // native.party_set_combo_charge(id, slot, charge) -> true | nil, message
    int nativePartySetComboCharge(lua_State* L)
    {
        const char* error = nullptr;
        if (!Party::setComboCharge(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), error))
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
    int nativeBuffClear(lua_State*)
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

    // native.buff_speeds(id) -> {[offset] = value} | nil: the stats block's speed values (attack, cast, movement)
    int nativeBuffSpeeds(lua_State* L)
    {
        std::vector<std::pair<uint16_t, int>> f;
        if (!Buffs::speeds(checkId(L, 1), f))
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

    int nativePartyExpScale(lua_State* L)
    {
        Party::setExperienceScale(luaL_checknumber(L, 1));
        return 0;
    }


    void registerParty(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"party_available", nativePartyAvailable},
            {"party_heroes", nativePartyHeroes},
            {"party_mercenaries", nativePartyMercenaries},
            {"party_recruit", nativePartyRecruit},
            {"party_dismiss", nativePartyDismiss},
            {"party_control", nativePartyControl},
            {"party_order", nativePartyOrder},
            {"party_abilities", nativePartyAbilities},
            {"party_combos", nativePartyCombos},
            {"party_set_combo", nativePartySetCombo},
            {"party_set_combo_charge", nativePartySetComboCharge},
            {"party_equipment", nativePartyEquipment},
            {"object_carried", nativeObjectCarried},
            {"party_state", nativePartyState},
            {"party_potion", nativePartyPotion},
            {"party_revive", nativePartyRevive},
            {"party_add_exp", nativePartyAddExp},
            {"party_hostile", nativePartyHostile},
            {"party_hero_files", nativePartyHeroFiles},
            {"party_import", nativePartyImport},
            {"party_export", nativePartyExport},
            {"party_set_name", nativePartySetName},
            {"party_portrait", nativePartyPortrait},
            {"buff_apply", nativeBuffApply},
            {"buff_remove", nativeBuffRemove},
            {"buff_list", nativeBuffList},
            {"buff_ended", nativeBuffEnded},
            {"buff_clear", nativeBuffClear},
            {"buff_fields", nativeBuffFields},
            {"buff_speeds", nativeBuffSpeeds},
            {"buff_overlay", nativeBuffOverlay},
            {"buff_set_overlay", nativeBuffSetOverlay},
            {"party_weapon_slots", nativePartyWeaponSlots},
            {"party_use_weapon_slot", nativePartyUseWeaponSlot},
            {"party_exp_range", nativePartyExpRange},
            {"party_shared_purse", nativePartySharedPurse},
            {"party_give_gold", nativePartyGiveGold},
            {"party_share", nativePartyShare},
            {"party_experience_scale", nativePartyExpScale},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
