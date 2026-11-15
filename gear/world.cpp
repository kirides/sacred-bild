#include "world.h"
#include "game/gear_addr.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using namespace Gear;

    // Object layout (ENG; see docs/Gear.md, "Engine notes"): +0x0C id, +0x10 type, +0x14 flags, +0x18 the world
    // position (WorldPos). Creatures: +0x3B4 experience, +0x3EE gold, +0x3FE level (the "External Data Transfer"
    // dump, 007F49A0, reads them so), +0x4D4 maximum and +0x4D8 current health (the hero portrait, 006D73D0).
    constexpr uintptr_t kId = 0x0C, kType = 0x10, kFlags = 0x14, kPos = 0x18;
    constexpr uintptr_t kExperience = 0x3B4, kGold = 0x3EE, kLevel = 0x3FE, kMaxHealth = 0x4D4, kHealth = 0x4D8;
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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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

    // 1: created (id in `id`), 0: the game returned 0, -1: fault.
    int callCreate(void* m, uint32_t type, WorldPos* pos, uint32_t& id)
    {
        __try
        {
            id = Addr::cObjectManager_create(m, type, pos, 0, 0, 0);
            return id ? 1 : 0;
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
        out = {p.x, p.y, p.layer};
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

bool World::changesAllowed()
{
    void* m = manager();
    if (!available() || !hero())
    {
        return false;
    }
    int heroes = 0;
    for (uint32_t id = 1; id <= kHeroIds; ++id)
    {
        const uintptr_t o = live(m, id);
        if (o && callIsCreature(o))
        {
            ++heroes;
        }
    }
    return heroes == 1;
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
    void* m = manager();
    if (!Addr::cObjectManager_create || !changesAllowed())
    {
        error = "world changes are not possible now (no single player game, or the address was not found)";
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
    const int result = callCreate(m, type, &pos, id);
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
    const int result = callPop(m, id, &pos);
    if (result <= 0)
    {
        error = result < 0 ? "the game faulted while moving the object" : "the game could not place the object there";
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

bool World::setGold(uint32_t id, uint32_t gold, const char*& error)
{
    void* m = manager();
    const uintptr_t o = changesAllowed() && id >= 1 && id <= kHeroIds ? live(m, id) : 0;
    if (!o || !callIsCreature(o))
    {
        error = "no such hero, or world changes are not possible now";
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
    return o && callIsCreature(o) && readAt(o, kType, type) && type >= 1 && type <= 9 ? type : 0;
}

uint32_t World::classBit(uint32_t heroClass)
{
    if (heroClass >= 1 && heroClass <= 6)
    {
        return 1u << (heroClass - 1);
    }
    return heroClass == 8 ? 0x400000u : heroClass == 9 ? 0x800000u : 0;
}