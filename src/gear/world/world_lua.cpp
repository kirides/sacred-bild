// The Lua host's natives for the world: hook switches, creatures and NPCs, abilities and effects, objects, spawning, teleports.
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
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

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

    // native.kill_recorded_active(bool): whether some mod handles `kill.recorded`
    int nativeKillRecordedActive(lua_State* L)
    {
        KillStat::setActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.potion_pick_active(bool): whether some mod handles `potion.pick`
    int nativePotionPickActive(lua_State* L)
    {
        PotionPick::setActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.usable_active(bool): whether some mod handles `rune.usable`
    int nativeUsableActive(lua_State* L)
    {
        Classes::setUsableActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.pfx_define{id, textures = {{from, to}, ...}, hue, colorize, multiply, alpha, speed} -> true | nil, message
    // native.pfx_undefine(id) -> bool; native.pfx_watch({fx, ...}, all)
    // native.pfx_stats() -> {live, applied}
    int nativePfxDefine(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TTABLE);
        Pfx::Variant v;
        lua_getfield(L, 1, "id");
        v.id = static_cast<uint32_t>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 1, "textures");
        if (lua_istable(L, -1))
        {
            const int n = static_cast<int>(lua_objlen(L, -1));
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i);
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                const char* from = lua_tostring(L, -2);
                const char* to = lua_tostring(L, -1);
                if (from && to)
                {
                    v.textures.emplace_back(from, to);
                }
                lua_pop(L, 3);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "hue");
        if (lua_isnumber(L, -1))
        {
            v.hasHue = true;
            v.hue = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "colorize");
        if (lua_isnumber(L, -1))
        {
            v.hasColorize = true;
            v.colorize = static_cast<uint32_t>(lua_tonumber(L, -1)) & 0xFFFFFF;
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "multiply");
        if (lua_isnumber(L, -1))
        {
            v.hasMultiply = true;
            v.multiply = static_cast<uint32_t>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "alpha");
        if (lua_isnumber(L, -1))
        {
            v.alpha = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "speed");
        if (lua_isnumber(L, -1))
        {
            v.speed = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        const char* error = nullptr;
        if (!Pfx::define(v, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    int nativePfxUndefine(lua_State* L)
    {
        lua_pushboolean(L, Pfx::undefine(static_cast<uint32_t>(luaL_checknumber(L, 1))));
        return 1;
    }

    int nativePfxWatch(lua_State* L)
    {
        std::vector<uint32_t> types;
        if (lua_istable(L, 1))
        {
            const int n = static_cast<int>(lua_objlen(L, 1));
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, 1, i);
                types.push_back(static_cast<uint32_t>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
        }
        Pfx::watch(types, lua_toboolean(L, 2) != 0);
        return 0;
    }

    int nativePfxStats(lua_State* L)
    {
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, static_cast<lua_Number>(Pfx::liveCount()));
        lua_setfield(L, -2, "live");
        lua_pushnumber(L, static_cast<lua_Number>(Pfx::appliedCount()));
        lua_setfield(L, -2, "applied");
        return 1;
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

    // native.text_key(key, utf8) -> id | nil, message: Gear's own text with an id taken from the key (the same in every session)
    int nativeTextKey(lua_State* L)
    {
        const char* error = nullptr;
        const uint32_t id = Classes::keyText(luaL_checkstring(L, 1), luaL_checkstring(L, 2), error);
        if (!id)
        {
            return pushFailure(L, error);
        }
        lua_pushnumber(L, id);
        return 1;
    }

    // native.text_key_of(id) -> the key | nil
    int nativeTextKeyOf(lua_State* L)
    {
        const std::string key = Classes::keyOfText(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        if (key.empty())
        {
            lua_pushnil(L);
        }
        else
        {
            lua_pushlstring(L, key.data(), key.size());
        }
        return 1;
    }

    // native.name_text(id) -> text id (0: the type's name) | nil; native.set_name_text(id, text) -> true | nil, message
    int nativeNameText(lua_State* L)
    {
        uint32_t text = 0;
        if (!World::nameText(checkId(L, 1), text))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, text);
        return 1;
    }
    int nativeSetNameText(lua_State* L)
    {
        const char* error = nullptr;
        if (!World::setNameText(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.clock() -> minutes since midnight | nil
    int nativeClock(lua_State* L)
    {
        uint32_t minutes = 0;
        if (!World::clockMinutes(minutes))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, minutes);
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

    // native.item_define_type(type, base, art, name_text) -> true | nil, message; native.item_use(creature id, item id) -> true | nil, message
    int nativeItemDefineType(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::defineItemType(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)),
                                     static_cast<uint32_t>(luaL_optnumber(L, 3, 0)), static_cast<uint32_t>(luaL_optnumber(L, 4, 0)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    int nativeItemUse(lua_State* L)
    {
        const char* error = nullptr;
        if (!Classes::useItem(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
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
            set("casting", o.casting);
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

    // native.copy_stats(id, from) -> true | nil, message
    int nativeCopyStats(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const uint32_t from = checkId(L, 2);
        const char* error = "no such creature";
        if (!id || !from || !World::copyStats(id, from, error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_level(id, level) -> true | nil, message
    int nativeSetLevel(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number level = luaL_checknumber(L, 2);
        const char* error = "no such creature";
        if (!id || !(level >= 1 && level <= 250) || !World::setLevel(id, static_cast<uint32_t>(level), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_pet(id, scale, untargetable) -> true | nil, message
    int nativeSetPet(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        const lua_Number scale = luaL_checknumber(L, 2);
        const bool untargetable = lua_toboolean(L, 3) != 0;
        const char* error = "no such creature";
        if (!id || !(scale > 0 && scale <= 4) || !World::setPet(id, scale, untargetable, error))
        {
            return pushFailure(L, scale > 0 && scale <= 4 ? error : "the scale is 0.1 to 4");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_tint(id, part, argb) -> true | nil, message
    int nativeSetTint(lua_State* L)
    {
        const char* error = nullptr;
        if (!World::setTint(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, 3))), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.set_texture(id, part, image) -> true | nil, message
    int nativeSetTexture(lua_State* L)
    {
        const char* error = nullptr;
        if (!World::setTexture(checkId(L, 1), static_cast<uint32_t>(luaL_checknumber(L, 2)), static_cast<uint32_t>(luaL_checknumber(L, 3)), error))
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.clear_pets()
    int nativeClearPets(lua_State*)
    {
        World::clearPets();
        return 0;
    }

    // native.gait(id, value?) -> gait | nil, message: a creature's gait (1 walk, 2 run); with a value it is set first.
    int nativeGait(lua_State* L)
    {
        const uint32_t id = checkId(L, 1);
        int32_t value = static_cast<int32_t>(luaL_optnumber(L, 2, 0));
        const bool set = lua_isnumber(L, 2) != 0;
        if (!id || (set && (value < 0 || value > 3)))
        {
            return pushFailure(L, "bad gait (0 to 3)");
        }
        if (!World::gait(id, value, set) || (set && !World::gait(id, value, false)))
        {
            return pushFailure(L, "no such creature");
        }
        lua_pushnumber(L, value);
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


    void registerWorld(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"master_active", nativeMasterActive},
            {"branch_active", nativeBranchActive},
            {"skillkey_active", nativeSkillKeyActive},
            {"skillslot_active", nativeSkillSlotActive},
            {"cursor_click", nativeCursorClick},
            {"bonus_active", nativeBonusActive},
            {"effect_active", nativeEffectActive},
            {"damage_active", nativeDamageActive},
            {"creature_ability_active", nativeCreatureAbilityActive},
            {"route_active", nativeRouteActive},
            {"motion_active", nativeMotionActive},
            {"creature_learn", nativeCreatureLearn},
            {"creature_forget", nativeCreatureForget},
            {"item_use_active", nativeItemUseActive},
            {"portals", nativePortals},
            {"open_teleporter", nativeOpenTeleporter},
            {"show_text", nativeShowText},
            {"creature_effect", nativeCreatureEffect},
            {"creature_raise", nativeCreatureRaise},
            {"kill_recorded_active", nativeKillRecordedActive},
            {"potion_pick_active", nativePotionPickActive},
            {"usable_active", nativeUsableActive},
            {"pfx_define", nativePfxDefine},
            {"pfx_undefine", nativePfxUndefine},
            {"pfx_watch", nativePfxWatch},
            {"pfx_stats", nativePfxStats},
            {"alias_create", nativeAliasCreate},
            {"alias_remove", nativeAliasRemove},
            {"alias_link", nativeAliasLink},
            {"linked_spell", nativeLinkedSpell},
            {"alias_clear", nativeAliasClear},
            {"alias_list", nativeAliasList},
            {"hero_record", nativeHeroRecord},
            {"hero_record_set", nativeHeroRecordSet},
            {"text_add", nativeTextAdd},
            {"text_key", nativeTextKey},
            {"text_key_of", nativeTextKeyOf},
            {"name_text", nativeNameText},
            {"set_name_text", nativeSetNameText},
            {"clock", nativeClock},
            {"texture_image", nativeTextureImage},
            {"world_cursor", nativeWorldCursor},
            {"art_in_flight", nativeArtInFlight},
            {"item_define_type", nativeItemDefineType},
            {"item_use", nativeItemUse},
            {"text_set", nativeTextSet},
            {"model_motion", nativeModelMotion},
            {"model_set_motion", nativeModelSetMotion},
            {"model_has_motion", nativeModelHasMotion},
            {"combat_constant_active", nativeCombatConstantActive},
            {"site_hooks", nativeSiteHooks},
            {"engine_free_slot", nativeEngineFreeSlot},
            {"engine_level_factor_start", nativeEngineLevelFactorStart},
            {"engine_level_factor_result", nativeEngineLevelFactorResult},
            {"hero", nativeHero},
            {"world_changes", nativeWorldChanges},
            {"object", nativeObject},
            {"creatures_near", nativeCreaturesNear},
            {"spawn", nativeSpawn},
            {"created_active", nativeCreatedActive},
            {"created_suppress", nativeCreatedSuppress},
            {"take_created", nativeTakeCreated},
            {"teleport", nativeTeleport},
            {"remove", nativeRemove},
            {"set_health", nativeSetHealth},
            {"copy_allegiance", nativeCopyAllegiance},
            {"copy_stats", nativeCopyStats},
            {"set_level", nativeSetLevel},
            {"set_pet", nativeSetPet},
            {"set_tint", nativeSetTint},
            {"set_texture", nativeSetTexture},
            {"clear_pets", nativeClearPets},
            {"set_max_health", nativeSetMaxHealth},
            {"set_gold", nativeSetGold},
            {"gait", nativeGait},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
