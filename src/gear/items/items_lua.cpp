// The Lua host's natives for items: shops, inventories, tooltips, pickup rules and drops.
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
    // native.shop_active(bool): whether some mod handles `shop.stock`
    int nativeShopActive(lua_State* L)
    {
        Shop::setActive(lua_toboolean(L, 1) != 0);
        return 0;
    }

    // native.shop_info() -> {open, merchant, hero_level, pages = {{container, visible, categories = {{category, weight}, ...}}, ...}} | nil
    int nativeShopInfo(lua_State* L)
    {
        Shop::Info info;
        if (!Shop::info(info))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushboolean(L, info.open ? 1 : 0);
        lua_setfield(L, -2, "open");
        lua_pushnumber(L, info.merchant);
        lua_setfield(L, -2, "merchant");
        lua_pushnumber(L, info.heroLevel);
        lua_setfield(L, -2, "hero_level");
        lua_createtable(L, static_cast<int>(info.pages.size()), 0);
        for (size_t p = 0; p < info.pages.size(); ++p)
        {
            const Shop::Page& page = info.pages[p];
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, page.container);
            lua_setfield(L, -2, "container");
            lua_pushboolean(L, page.visible ? 1 : 0);
            lua_setfield(L, -2, "visible");
            lua_createtable(L, static_cast<int>(page.categories.size()), 0);
            for (size_t i = 0; i < page.categories.size(); ++i)
            {
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, page.categories[i].type);
                lua_setfield(L, -2, "category");
                lua_pushnumber(L, page.categories[i].weight);
                lua_setfield(L, -2, "weight");
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            lua_setfield(L, -2, "categories");
            lua_rawseti(L, -2, static_cast<int>(p + 1));
        }
        lua_setfield(L, -2, "pages");
        return 1;
    }

    // native.shop_pick(category, level, quality, hero_pref) -> item type | nil
    int nativeShopPick(lua_State* L)
    {
        const uint32_t type = Shop::pick(static_cast<uint32_t>(luaL_checknumber(L, 1)), static_cast<uint32_t>(luaL_checknumber(L, 2)),
                                         static_cast<uint32_t>(luaL_optnumber(L, 3, 0)), static_cast<uint32_t>(luaL_optnumber(L, 4, 0)));
        if (type)
        {
            lua_pushnumber(L, type);
        }
        else
        {
            lua_pushnil(L);
        }
        return 1;
    }

    // native.shop_add(container, type, quality, level) -> item id | nil, message
    int nativeShopAdd(lua_State* L)
    {
        uint32_t id = 0;
        const char* error = "bad arguments";
        if (Shop::add(static_cast<uint32_t>(luaL_optnumber(L, 1, 0)), static_cast<uint32_t>(luaL_checknumber(L, 2)),
                      static_cast<uint32_t>(luaL_optnumber(L, 3, 0)), static_cast<uint32_t>(luaL_optnumber(L, 4, 0)), id, error))
        {
            lua_pushnumber(L, id);
            return 1;
        }
        return pushFailure(L, error);
    }

    // native.shop_remove(container, item id) -> true | nil, message
    int nativeShopRemove(lua_State* L)
    {
        const char* error = "bad arguments";
        if (Shop::remove(static_cast<uint32_t>(luaL_optnumber(L, 1, 0)), checkId(L, 2), error))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
        return pushFailure(L, error);
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

    // Drops (src/gear/items/drops.h). native.drops_handlers(roll, select, random); native.drops_scales(chance, find); native.drops_rng(mode, seeded, seed);
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


    void registerItems(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"shop_active", nativeShopActive},
            {"shop_info", nativeShopInfo},
            {"shop_pick", nativeShopPick},
            {"shop_add", nativeShopAdd},
            {"shop_remove", nativeShopRemove},
            {"item", nativeItem},
            {"items_near", nativeItemsNear},
            {"hero_class", nativeHeroClass},
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
            {"item_hover", nativeItemHover},
            {"item_bonus_text", nativeItemBonusText},
            {"item_equip_slot", nativeItemEquipSlot},
            {"item_type_text", nativeItemTypeText},
            {"inv_export", nativeInvExport},
            {"inv_import", nativeInvImport},
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
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
