#include "memfault.h"
#include "world.h"
#include "game/gear_addr.h"
#include "log.h"
#include "netmsg.h"
#include "patch.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    using namespace Gear;

    // Object layout (ENG; see docs/RE_NOTES.md, "Objects and creatures"): +0x0C id, +0x10 type, +0x14 flags, +0x18 the world
    // position (WorldPos). Creatures: +0x3B4 experience, +0x3EE gold, +0x3FE level (the "External Data Transfer"
    // dump, 007F49A0, reads them so), +0x4D4 maximum and +0x4D8 current health (the hero portrait, 006D73D0).
    constexpr uintptr_t kId = 0x0C, kType = 0x10, kFlags = 0x14, kPos = 0x18;
    constexpr uintptr_t kFaction = 0x1F0, kBehavior = 0x1F4;   // the relation class (isEnemy, ENG 00423580) and the behavior flags
    constexpr uintptr_t kExperience = 0x3B4, kGold = 0x3EE, kLevel = 0x3FE, kMaxHealth = 0x4D4, kHealth = 0x4D8, kState = 0x200;
    constexpr uint32_t kFlagGone = 0x10;    // set on objects the manager freed or parked (create and pop skip them)
    constexpr uint32_t kTypeCount = 0x7E60; // the type records (00425EA0 accepts 0 < type < 0x7E60)
    constexpr uint32_t kHeroIds = 16;       // ids 1..16 are the players' heroes (007F49A0 walks them)

    // The game's position struct (006224E0 copies it field by field): the sector the map files the spot under (0:
    // let the engine find it, as the hero's creation does), x, y, the layer.
    struct WorldPos
    {
        uint16_t sector;
        uint16_t _02;
        int32_t x;
        int32_t y;
        uint8_t layer;
        uint8_t _0D[3];
    };
    static_assert(sizeof(WorldPos) == 0x10);

    bool safeCopy(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (Gear::isMemoryFault(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool safeStore(void* dst, const void* src, size_t size)
    {
        return safeCopy(dst, src, size);
    }

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    void* manager()
    {
        void* m = nullptr;
        if (!Addr::g_pObjectManager || !safeCopy(&m, Addr::g_pObjectManager.get(), sizeof(m)))
        {
            return nullptr;
        }
        return m;
    }

    // The mod-facing calls below run under a fault guard: the engine thread is the game's, and a fault in a call a
    // mod asked for must not take the game down. No C++ objects in these functions (__try needs that).
    bool callIsCreature(uintptr_t object)
    {
        __try
        {
            return Addr::rtDynamicCast(reinterpret_cast<void*>(object), 0, Addr::cObject_typeDescriptor.get(),
                                       Addr::cCreature_typeDescriptor.get(), 0) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t callHero(void* m)
    {
        __try
        {
            return reinterpret_cast<uintptr_t>(Addr::cObjectManager_hero(m));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // --- creations the engine itself makes (creature.created) ---------------------------------------------------------------------
    // The engine's create (0x005FBA40) is hooked so that mods can see the creatures it spawns by itself (sector fills, spawn
    // tables, scripts). The hook only records {id, type, position} while a mod listens; the Lua side drains the list on the next
    // tick, when the creature is complete. Creations made by Gear (`callCreate`), by a savegame or network read (a wanted id)
    // and the heroes are left out.
    using CreateFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t type, WorldPos* pos, int want, int broadcast, int extra);
    CreateFn g_origCreate = nullptr;
    std::atomic<bool> g_createdActive{false};
    thread_local bool t_ownCreate = false;
    std::atomic<bool> g_createdSuppress{false};   // while `creature.created` handlers run: what they make (by any path) is not reported back
    std::mutex g_createdLock;
    std::vector<World::Created> g_created;
    uint64_t g_createdDropped = 0;
    constexpr size_t kCreatedMax = 2048;

    uint32_t __fastcall hookCreate(void* self, void* edx, uint32_t type, WorldPos* pos, int want, int broadcast, int extra)
    {
        const uint32_t id = g_origCreate(self, edx, type, pos, want, broadcast, extra);
        if (id > kHeroIds && !want && !t_ownCreate && !g_createdSuppress.load(std::memory_order_relaxed) && g_createdActive.load(std::memory_order_relaxed))
        {
            World::Created c{id, type, 0, 0, 0};
            WorldPos p{};
            if (pos && safeCopy(&p, pos, sizeof(p)))
            {
                c.x = p.x;
                c.y = p.y;
                c.layer = p.layer;
            }
            std::lock_guard<std::mutex> lock(g_createdLock);
            if (g_created.size() < kCreatedMax)
            {
                g_created.push_back(c);
            }
            else
            {
                ++g_createdDropped;
            }
        }
        return id;
    }

    // 1: created (id in `id`), 0: the game returned 0, -1: fault.
    // `want`: the id the object should get (0: the next free one; a taken id's object is destroyed first, as the
    // engine's create does).
    int callCreate(void* m, uint32_t type, WorldPos* pos, bool broadcast, uint32_t want, uint32_t& id)
    {
        t_ownCreate = true;
        __try
        {
            id = Addr::cObjectManager_create(m, type, pos, static_cast<int>(want), broadcast ? 1 : 0, 0);
            t_ownCreate = false;
            return id ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t_ownCreate = false;
            return -1;
        }
    }

    // The engine's gold setter (ENG 0x580110), read from the call at cStats_setGoldCall; 0 if that is not a call.
    Sacred::Thiscall<void(void* stats, uint32_t gold)> goldSetter()
    {
        static uintptr_t target = [] {
            const uintptr_t at = Addr::cStats_setGoldCall;
            int32_t rel = 0;
            if (!at || !Patch::verify(at, {0xE8}))
            {
                return uintptr_t{0};
            }
            std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), 4);
            return static_cast<uintptr_t>(at + 5 + rel);
        }();
        return {target};
    }

    // The engine's own gold setter on the hero's stats (+0x3A8): -1 on a fault.
    int callSetGold(uintptr_t hero, uint32_t gold)
    {
        __try
        {
            goldSetter()(reinterpret_cast<void*>(hero + 0x3A8), gold);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callPop(void* m, uint32_t id, WorldPos* pos)
    {
        __try
        {
            return (Addr::cObjectManager_pop(m, id, pos) & 0xFF) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callUnlink(void* m, uint32_t id)
    {
        __try
        {
            Addr::cObjectManager_unlink(m, id);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callDestroyObject(void* m, uint32_t id)
    {
        __try
        {
            Addr::cObjectManager_destroy(m, id, 1, 0, 0);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The number of slots in the manager's id vector (+4 begin, +8 end), and where the vector is.
    uint32_t slots(void* m, uintptr_t* first = nullptr)
    {
        uintptr_t range[2] = {};
        if (!m || !safeCopy(range, static_cast<const uint8_t*>(m) + 4, sizeof(range)) || range[1] < range[0])
        {
            return 0;
        }
        if (first)
        {
            *first = range[0];
        }
        return static_cast<uint32_t>(std::min<uintptr_t>((range[1] - range[0]) / 4, 0x1000000));
    }

    // The live object with `id`, 0 if there is none (or it was freed / parked). Reads the manager's vector the way
    // getData (005FE000) does, without its repair of a mismatched id.
    uintptr_t live(void* m, uint32_t id)
    {
        uintptr_t first = 0;
        if (!m || !id || (id & 0x80000000u) || id >= slots(m, &first))
        {
            return 0;
        }
        uintptr_t o = 0;
        uint32_t flags = 0, ownId = 0;
        if (!readAt(first, id * 4, o) || !o || !readAt(o, kFlags, flags) || !readAt(o, kId, ownId) || ownId != id ||
            (flags & kFlagGone))
        {
            return 0;
        }
        return o;
    }

    bool readPos(uintptr_t o, World::Position& out)
    {
        WorldPos p{};
        if (!readAt(o, kPos, p))
        {
            return false;
        }
        out = {p.x, p.y, p.layer, p.sector};
        return true;
    }

    // Items. The type records (*g_pTypeManager + type * 0x80): +0x00 flags (bit 24: quest item, 0x00428680), +0x18
    // the item record's index, +0x2E the kind. Item records: *(typeManager + 0x563004) + index * 0x102, count at
    // +0x563000 (00434030); +0x23 nonzero: a set item. An item object (cItemBase) keeps a copy of its record's second
    // half at +0x118; its dword +0x11C holds the class mask (bits 0-5, 22, 23) and the rarity (bits 16-19), as the
    // label colour (005C13C0) reads them. Gold: types 0x140C-0x140F (0043AF50).
    constexpr uintptr_t kItemData = 0x11C;
    constexpr uintptr_t kTypeRecordSize = 0x80, kTypeKind = 0x2E, kTypeItemIndex = 0x18;
    constexpr uintptr_t kItemCount = 0x563000, kItemRecords = 0x563004, kItemRecordSize = 0x102, kItemSet = 0x23;
    constexpr uint32_t kFlagOwned = 0x02;   // not lying in the world (in an inventory, being picked up)

    bool callIsItem(uintptr_t object)
    {
        __try
        {
            return Addr::cItemBase_typeDescriptor &&
                   Addr::rtDynamicCast(reinterpret_cast<void*>(object), 0, Addr::cObject_typeDescriptor.get(),
                                       Addr::cItemBase_typeDescriptor.get(), 0) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t typeManager()
    {
        void* tm = nullptr;
        return Addr::g_pTypeManager && safeCopy(&tm, Addr::g_pTypeManager.get(), sizeof(tm)) ? reinterpret_cast<uintptr_t>(tm) : 0;
    }
    // A spot in the world: inside the 128 x 128 sectors of 64 tiles each that the map indexes (00635C40).
    bool validSpot(const World::Position& at)
    {
        constexpr int32_t kLimit = 128 * 64 * 54;
        return at.x >= 0 && at.y >= 0 && at.x < kLimit && at.y < kLimit;
    }
}

bool World::available()
{
    return Addr::g_pObjectManager && Addr::cObjectManager_hero && Addr::rtDynamicCast &&
           Addr::cObject_typeDescriptor && Addr::cCreature_typeDescriptor && manager();
}

bool World::networkGame()
{
    // 0x0182EBEC is the byte after the variable the signature finds (0x0182EBE8). Unknown: treat as a network game.
    uint8_t flag = 1;
    return !Addr::g_pNetPlayers || !safeCopy(&flag, reinterpret_cast<const void*>(Addr::g_pNetPlayers.address + 4), 1) ||
           flag != 0;
}

namespace
{
    ULONGLONG g_netSince = 0;       // when a game entered through the Internet menu had its hero (0: not yet)
    bool g_testProven = false;      // self tests: Open Net counts as proven
    constexpr ULONGLONG kProveMs = 10000;
}

World::NetworkMode World::networkMode()
{
    if (!networkGame())
    {
        g_netSince = 0;
        NetMsg::clearHeroFromServer();
        return NetworkMode::Single;
    }
    uint32_t net = 0;
    uint8_t internet = 0, identity = 0;
    if (!Addr::g_pNetPlayers || !safeCopy(&net, reinterpret_cast<const void*>(Addr::g_pNetPlayers.address), 4) || !net ||
        !safeCopy(&internet, reinterpret_cast<const void*>(static_cast<uintptr_t>(net) + 0x3818), 1) ||
        !safeCopy(&identity, reinterpret_cast<const void*>(static_cast<uintptr_t>(net) + 0x3819), 1))
    {
        return NetworkMode::Closed;     // a network game whose kind cannot be read is treated as the strictest
    }
    if (NetMsg::heroFromServer())
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG("World: the game server delivered the hero (message 0x132): this is a Closed Net game");
        }
        return NetworkMode::Closed;
    }
    if (identity)
    {
        return NetworkMode::Closed;
    }
    if (!internet)
    {
        return NetworkMode::Lan;        // entered through the LAN menu
    }
    // Entered through the Internet menu (Closed or Open Net): Closed Net unless Open Net is proven. The hero comes from the
    // server in Closed Net (message 0x132, right after admission); in Open Net the client keeps its local hero and never
    // gets one. Proof of Open Net: the receive hook is in, the hero has been in the game for kProveMs, no 0x132 came.

    if (g_testProven)
    {
        return NetworkMode::Open;
    }
    if (!NetMsg::heroSourceKnown())
    {
        return NetworkMode::Closed;
    }
    uint32_t hero = 0;
    if (!safeCopy(&hero, reinterpret_cast<const void*>(static_cast<uintptr_t>(net) + 0x14), 4) || hero == 0)
    {
        g_netSince = 0;
        return NetworkMode::Closed;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_netSince)
    {
        g_netSince = now;
    }
    return now - g_netSince >= kProveMs ? NetworkMode::Open : NetworkMode::Closed;
}

bool World::setNetworkModeForTest(NetworkMode mode, bool proven)
{
    uint32_t net = 0;
    const uint8_t flag = mode == NetworkMode::Single ? 0 : 1;
    const uint8_t internet = mode == NetworkMode::Open || mode == NetworkMode::Closed ? 1 : 0;
    const uint8_t identity = mode == NetworkMode::Closed ? 1 : 0;
    g_testProven = proven && mode == NetworkMode::Open;
    return Addr::g_pNetPlayers && safeCopy(&net, reinterpret_cast<const void*>(Addr::g_pNetPlayers.address), 4) && net &&
           safeStore(reinterpret_cast<void*>(Addr::g_pNetPlayers.address + 4), &flag, 1) &&
           safeStore(reinterpret_cast<void*>(static_cast<uintptr_t>(net) + 0x3818), &internet, 1) &&
           safeStore(reinterpret_cast<void*>(static_cast<uintptr_t>(net) + 0x3819), &identity, 1);
}

bool World::setNetworkFlagForTest(bool on)
{
    const uint8_t flag = on ? 1 : 0;
    return Addr::g_pNetPlayers && safeStore(reinterpret_cast<void*>(Addr::g_pNetPlayers.address + 4), &flag, 1);
}

bool World::changesAllowed()
{
    void* m = manager();
    if (!available() || !hero() || networkGame())
    {
        return false;
    }
    // Another player's hero (one the local player does not own, +0x39) means a network game whatever the flag says.
    // The party's companions (gear/party.cpp) are heroes of the local player.
    const uint32_t local = localId();
    int foreign = 0;
    for (uint32_t id = 1; id <= kHeroIds; ++id)
    {
        const uintptr_t o = live(m, id);
        uint8_t owner = 0;
        if (o && callIsCreature(o) && readAt(o, 0x39, owner) && owner != local)
        {
            ++foreign;
        }
    }
    return foreign == 0;
}

uint32_t World::localId()
{
    uint32_t net = 0, id = 0;
    return Addr::g_pNetPlayers && safeCopy(&net, reinterpret_cast<const void*>(Addr::g_pNetPlayers.address), 4) && net &&
                   readAt(net, 0x14, id)
               ? id
               : 0;
}

bool World::destroyObject(uint32_t id)
{
    void* m = manager();
    return Addr::cObjectManager_destroy && live(m, id) && callDestroyObject(m, id) > 0;
}

uint32_t World::hero()
{
    void* m = manager();
    if (!available() || !m)
    {
        return 0;
    }
    const uintptr_t o = callHero(m);
    uint32_t id = 0;
    return o && readAt(o, kId, id) && live(m, id) == o ? id : 0;
}

bool World::object(uint32_t id, Object& out)
{
    void* m = available() ? manager() : nullptr;
    const uintptr_t o = live(m, id);
    if (!o)
    {
        return false;
    }
    Object r{};
    r.id = id;
    if (!readAt(o, kType, r.type) || !readAt(o, kFlags, r.flags) || !readPos(o, r.position))
    {
        return false;
    }
    r.creature = callIsCreature(o);
    if (r.creature)
    {
        uint16_t level = 0;
        if (!readAt(o, kHealth, r.health) || !readAt(o, kMaxHealth, r.maxHealth) || !readAt(o, kGold, r.gold) ||
            !readAt(o, kExperience, r.experience) || !readAt(o, kLevel, level))
        {
            return false;
        }
        r.level = level;
        uint32_t state = 0;
        r.inCombat = readAt(o, kState, state) && (state & 0x8000) != 0;
        uint16_t casting = 0;
        r.casting = readAt(o, 0x120, casting) ? casting : 0;
    }
    out = r;
    return true;
}

std::vector<uint32_t> World::creaturesNear(const Position& at, double radius, size_t limit)
{
    std::vector<std::pair<double, uint32_t>> found;
    void* m = available() ? manager() : nullptr;
    const uint32_t n = slots(m);
    const double r2 = radius * radius;
    for (uint32_t id = 1; id < n; ++id)
    {
        const uintptr_t o = live(m, id);
        Position p;
        if (!o || !readPos(o, p) || p.layer != at.layer)
        {
            continue;
        }
        const double dx = static_cast<double>(p.x) - at.x, dy = static_cast<double>(p.y) - at.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= r2 && callIsCreature(o))
        {
            found.emplace_back(d2, id);
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < found.size() && i < limit; ++i)
    {
        ids.push_back(found[i].second);
    }
    return ids;
}

uint32_t World::spawn(uint32_t type, const Position& at, const char*& error)
{
    if (!Addr::cObjectManager_create || !changesAllowed())
    {
        error = "world changes are not possible now (no single player game, or the address was not found)";
        return 0;
    }
    return createObject(type, at, false, error);
}

bool World::inventoryChangesAllowed()
{
    return available() && hero() && (networkGame() || changesAllowed());
}

uint32_t World::createObject(uint32_t type, const Position& at, bool broadcast, const char*& error, uint32_t want)
{
    void* m = manager();
    if (!Addr::cObjectManager_create || !available())
    {
        error = "the object functions were not found";
        return 0;
    }
    if (type == 0 || type >= kTypeCount)
    {
        error = "no such type";
        return 0;
    }
    if (!validSpot(at))
    {
        error = "the position is outside the world";
        return 0;
    }
    WorldPos pos{};
    pos.x = at.x;
    pos.y = at.y;
    pos.layer = at.layer;
    uint32_t id = 0;
    const int result = callCreate(m, type, &pos, broadcast, want, id);
    if (result < 0)
    {
        LOG("World: creating type {} at {},{} faulted", type, at.x, at.y);
        error = "the game faulted while creating the object";
        return 0;
    }
    if (!result || !live(m, id))
    {
        error = "the game did not create the object";
        return 0;
    }
    return id;
}

bool World::teleport(uint32_t id, const Position& at, const char*& error)
{
    void* m = manager();
    if (!Addr::cObjectManager_pop || !changesAllowed())
    {
        error = "world changes are not possible now";
        return false;
    }
    if (!live(m, id))
    {
        error = "no such object";
        return false;
    }
    if (!validSpot(at))
    {
        error = "the position is outside the world";
        return false;
    }
    WorldPos pos{};
    pos.x = at.x;
    pos.y = at.y;
    pos.layer = at.layer;
    // pop places only objects out of the world (+0x14 bit 2): take a placed one out first, and back where it was if
    // the new spot does not take it.
    uintptr_t o = live(m, id);
    uint32_t flags = 0;
    WorldPos old{};
    const bool placed = o && readAt(o, kFlags, flags) && !(flags & 2) && readAt(o, kPos, old);
    if (placed && (!Addr::cObjectManager_unlink || callUnlink(m, id) < 0))
    {
        error = "the game's object functions were not found";
        return false;
    }
    const int result = callPop(m, id, &pos);
    if (result <= 0 && placed)
    {
        callPop(m, id, &old);
    }
    if (result <= 0)
    {
        error = result < 0 ? "the game faulted while moving the object" : "the game could not place the object there";
        return false;
    }
    return true;
}

bool World::remove(uint32_t id, const char*& error)
{
    void* m = manager();
    if (!Addr::cObjectManager_destroy || !changesAllowed())
    {
        error = "world changes are not possible now";
        return false;
    }
    if (id <= kHeroIds)
    {
        error = "a hero cannot be removed";
        return false;
    }
    const uintptr_t o = live(m, id);
    if (!o || !callIsCreature(o))
    {
        error = "no such creature";
        return false;
    }
    if (callDestroyObject(m, id) < 0)
    {
        error = "the game faulted while removing the creature";
        return false;
    }
    return true;
}

bool World::setHealth(uint32_t id, int32_t health, const char*& error)
{
    void* m = manager();
    const uintptr_t o = changesAllowed() ? live(m, id) : 0;
    int32_t maxHealth = 0;
    if (!o || !callIsCreature(o) || !readAt(o, kMaxHealth, maxHealth))
    {
        error = "no such creature, or world changes are not possible now";
        return false;
    }
    health = std::clamp<int32_t>(health, 1, std::max<int32_t>(maxHealth, 1));
    if (!safeStore(reinterpret_cast<void*>(o + kHealth), &health, sizeof(health)))
    {
        error = "cannot write the creature";
        return false;
    }
    return true;
}

bool World::copyAllegiance(uint32_t id, uint32_t from, const char*& error)
{
    void* m = manager();
    const uintptr_t o = changesAllowed() ? live(m, id) : 0;
    const uintptr_t s = o ? live(m, from) : 0;
    uint32_t faction = 0, behavior = 0, own = 0;
    if (!o || !s || !callIsCreature(o) || !callIsCreature(s) || !readAt(s, kFaction, faction) || !readAt(s, kBehavior, behavior) ||
        !readAt(o, kBehavior, own))
    {
        error = "no such creature, or world changes are not possible now";
        return false;
    }
    if (faction > 15)
    {
        error = "the source has no valid faction";
        return false;
    }
    own = (own & 0xFFFF0000u) | (behavior & 0xFFFFu);
    if (!safeStore(reinterpret_cast<void*>(o + kFaction), &faction, sizeof(faction)) ||
        !safeStore(reinterpret_cast<void*>(o + kBehavior), &own, sizeof(own)))
    {
        error = "cannot write the creature";
        return false;
    }
    return true;
}

bool World::setMaxHealth(uint32_t id, int32_t maxHealth, bool setCurrent, int32_t health, const char*& error)
{
    void* m = manager();
    const uintptr_t o = changesAllowed() ? live(m, id) : 0;
    int32_t oldMax = 0, oldHealth = 0;
    if (!o || !callIsCreature(o) || !readAt(o, kMaxHealth, oldMax) || !readAt(o, kHealth, oldHealth))
    {
        error = "no such creature, or world changes are not possible now";
        return false;
    }
    if (id >= 1 && id <= kHeroIds)
    {
        error = "heroes keep the maximum their stats give";
        return false;
    }
    maxHealth = std::max<int32_t>(maxHealth, 1);
    if (!setCurrent)
    {
        health = oldMax > 0 ? static_cast<int32_t>(std::min<double>(static_cast<double>(oldHealth) / oldMax, 1.0) * maxHealth) : maxHealth;
    }
    health = std::clamp<int32_t>(health, 1, maxHealth);
    if (!safeStore(reinterpret_cast<void*>(o + kMaxHealth), &maxHealth, sizeof(maxHealth)) ||
        !safeStore(reinterpret_cast<void*>(o + kHealth), &health, sizeof(health)))
    {
        error = "cannot write the creature";
        return false;
    }
    return true;
}

bool World::setGold(uint32_t id, uint32_t gold, const char*& error)
{
    // Only the local hero, also in network games: the engine's setter (ENG 0x580110, what merchants and quests use)
    // runs its anti-cheat check, keeps the hero's checked copy of its values in step and marks the values dirty, so
    // the hero's next update sends them to the server and the peers (message 0x11B). A direct write would leave the
    // copy behind, and the engine's next check would take the hero for a cheater.
    void* m = available() ? manager() : nullptr;
    const uint32_t local = hero();
    const uintptr_t o = m && id >= 1 && id <= kHeroIds && id == local ? live(m, id) : 0;
    if (!o || !callIsCreature(o))
    {
        error = "only the local hero's gold can be changed, and only in a running game";
        return false;
    }
    gold = std::min<uint32_t>(gold, 0x7FFFFFFE);    // the engine's own limit
    if (goldSetter())
    {
        if (callSetGold(o, gold) < 0)
        {
            error = "the game's gold function failed";
            return false;
        }
        return true;
    }
    if (networkGame() || !changesAllowed())
    {
        error = "the game's gold function was not found (network games need it)";
        return false;
    }
    if (!safeStore(reinterpret_cast<void*>(o + kGold), &gold, sizeof(gold)))
    {
        error = "cannot write the hero";
        return false;
    }
    return true;
}

bool World::item(uint32_t id, Item& out)
{
    void* m = available() ? manager() : nullptr;
    const uintptr_t o = live(m, id);
    const uintptr_t tm = typeManager();
    if (!o || !tm || !callIsItem(o))
    {
        return false;
    }
    Item r{};
    r.id = id;
    uint32_t data = 0;
    if (!readAt(o, kType, r.type) || !readAt(o, kFlags, r.flags) || !readPos(o, r.position) ||
        !readAt(o, kItemData, data) || r.type == 0 || r.type >= kTypeCount)
    {
        return false;
    }
    const uintptr_t type = tm + r.type * kTypeRecordSize;
    uint16_t index = 0;
    if (!readAt(type, 0, r.typeFlags) || !readAt(type, kTypeKind, r.kind) || !readAt(type, kTypeItemIndex, index))
    {
        return false;
    }
    uint32_t count = 0;
    uintptr_t records = 0;
    uint8_t set = 0;
    if (readAt(tm, kItemCount, count) && readAt(tm, kItemRecords, records) && records && index < count &&
        readAt(records, index * kItemRecordSize + kItemSet, set))
    {
        r.setItem = set != 0;
    }
    r.onGround = !(r.flags & (kFlagOwned | kFlagGone));
    r.rarity = static_cast<uint8_t>((data >> 16) & 0xF);
    r.classMask = data & kAllClasses;
    r.gold = r.type >= 0x140C && r.type <= 0x140F;
    out = r;
    return true;
}

uintptr_t World::itemObject(uint32_t id)
{
    void* m = available() ? manager() : nullptr;
    const uintptr_t o = live(m, id);
    return o && callIsItem(o) ? o : 0;
}

std::vector<uint32_t> World::itemsNear(const Position& at, double radius, size_t limit)
{
    std::vector<std::pair<double, uint32_t>> found;
    void* m = available() ? manager() : nullptr;
    const uint32_t n = slots(m);
    const double r2 = radius * radius;
    for (uint32_t id = kHeroIds + 1; id < n; ++id)
    {
        const uintptr_t o = live(m, id);
        Position p;
        uint32_t flags = 0;
        if (!o || !readAt(o, kFlags, flags) || (flags & kFlagOwned) || !readPos(o, p) || p.layer != at.layer)
        {
            continue;
        }
        const double dx = static_cast<double>(p.x) - at.x, dy = static_cast<double>(p.y) - at.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= r2 && callIsItem(o))
        {
            found.emplace_back(d2, id);
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < found.size() && i < limit; ++i)
    {
        ids.push_back(found[i].second);
    }
    return ids;
}

uint32_t World::heroClass(uint32_t id)
{
    void* m = available() ? manager() : nullptr;
    const uintptr_t o = id >= 1 && id <= kHeroIds ? live(m, id) : 0;
    uint32_t type = 0;
    if (!o || !callIsCreature(o) || !readAt(o, kType, type) || type < 1 || type > 9)
    {
        return 0;
    }
    // Type 7 is a Vampiress in her vampire form (Vampirform, ability 1022) until she changes back: still a Vampiress.
    return type == 7 ? 6 : type;
}

uint32_t World::classBit(uint32_t heroClass)
{
    if (heroClass >= 1 && heroClass <= 6)
    {
        return 1u << (heroClass - 1);
    }
    return heroClass == 8 ? 0x400000u : heroClass == 9 ? 0x800000u : 0;
}

uintptr_t World::objectAddress(uint32_t id)
{
    return World::available() ? live(manager(), id) : 0;
}

bool World::carried(uint32_t id, uint32_t (&slots)[19])
{
    const uintptr_t o = objectAddress(id);
    return o && callIsCreature(o) && readAt(o, 0x1A4, slots);
}

bool World::installCreateHook()
{
    if (!Addr::cObjectManager_create)
    {
        LOG("World: the object create function was not found: creature.created is off");
        return false;
    }
    return Patch::hook(g_origCreate, Addr::cObjectManager_create, hookCreate, "cObjectManager_create");
}

void World::setCreatedSuppress(bool on)
{
    g_createdSuppress.store(on, std::memory_order_relaxed);
}

void World::setCreatedActive(bool on)
{
    g_createdActive.store(on, std::memory_order_relaxed);
    if (!on)
    {
        std::lock_guard<std::mutex> lock(g_createdLock);
        g_created.clear();
    }
}

std::vector<World::Created> World::takeCreated(uint64_t* dropped)
{
    std::vector<Created> out;
    std::lock_guard<std::mutex> lock(g_createdLock);
    out.swap(g_created);
    if (dropped)
    {
        *dropped = g_createdDropped;
    }
    return out;
}
