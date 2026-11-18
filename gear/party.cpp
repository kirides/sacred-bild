#include "party.h"
#include "inventory.h"
#include "log.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// Party companions (docs/Gear.md, "Party"; RE notes in docs/RE_NOTES.md, "Party").
//
// A network game holds up to 16 heroes (object ids 1..16), each played on the computer whose id the players object
// keeps as the local one (+0x14); an object ticks (walks, fights, runs its AI) only on the computer whose id is its
// owner byte (+0x39). In single player the local id is 1 and the player owns everything. A companion is one more
// hero of the local player, made as the engine makes the player's own. Switching control moves the local id and
// the ownership of everything the old id owned to the new hero: the UI, the camera, the inventory (backpack
// container n is the hero with id n), the mouse and the keys then serve it, and the world goes on ticking.
namespace
{
    using namespace Gear;

    constexpr uintptr_t kId = 0x0C, kType = 0x10, kFlags = 0x14, kPos = 0x18, kOwner = 0x39;
    constexpr uintptr_t kAction = 0xFC, kMovement = 0x150;
    constexpr uintptr_t kMaster = 0x251;           // a mercenary's leader (int, unaligned)
    constexpr uintptr_t kHome = 0x2B9;             // the creature's home position (the engine copies +0x18 there)
    constexpr uintptr_t kFollowers = 0x39C;        // vector {begin, end, capacity} of 9-byte {u32 id, u32, u8 active}
    constexpr uintptr_t kStats = 0x3A8;            // the stats block every creature has
    constexpr uintptr_t kExperience = 0x3B4, kGold = 0x3EE, kLevel = 0x3FE, kHealth = 0x4D8;
    constexpr uintptr_t kAbilities = kStats + 0x102;   // vector {begin, end} of 0x16-byte entries
    constexpr uintptr_t kAbilityId = 4, kAbilityLevel = 6, kAbilityBonus = 7, kAbilityRegenMax = 0x0A,
                        kAbilityRegen = 0x12, kAbilitySize = 0x16;
    constexpr uintptr_t kShadowCopy = 0x3B0;       // the stats words the anti-cheat shadow keeps (0x3B dwords + 1 word)
    constexpr uint32_t kFlagGone = 0x10;
    constexpr uintptr_t kEngineControlled = 0x13C; // the creature the world mouse handler commands
    constexpr uintptr_t kPlayersLocal = 0x14;

    // Creature event types (the 0x44-byte orders a creature's vtable +0x18 takes).
    constexpr uint32_t kOrderMove = 4, kOrderAttack = 7, kOrderAbility = 0x105;

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

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    template <class T>
    bool writeAt(uintptr_t base, uintptr_t offset, const T& value)
    {
        return base && safeCopy(reinterpret_cast<void*>(base + offset), &value, sizeof(T));
    }

    uintptr_t engine()
    {
        uintptr_t e = 0;
        return Addr::g_uiEventTarget && safeCopy(&e, Addr::g_uiEventTarget.get(), sizeof(e)) ? e : 0;
    }

    uintptr_t players()
    {
        uintptr_t p = 0;
        return Addr::g_pNetPlayers && safeCopy(&p, Addr::g_pNetPlayers.get(), sizeof(p)) ? p : 0;
    }

    // The object vector of the manager: [begin, end).
    bool objectRange(uintptr_t*& begin, uintptr_t*& end)
    {
        uintptr_t m = 0, range[2] = {};
        if (!Addr::g_pObjectManager || !safeCopy(&m, Addr::g_pObjectManager.get(), sizeof(m)) || !m ||
            !safeCopy(range, reinterpret_cast<const void*>(m + 4), sizeof(range)) || range[1] < range[0])
        {
            return false;
        }
        begin = reinterpret_cast<uintptr_t*>(range[0]);
        end = reinterpret_cast<uintptr_t*>(range[1]);
        return true;
    }

    // A hero object: a live creature with a hero type at id 1..16.
    uintptr_t heroObject(uint32_t id)
    {
        if (id < 1 || id > Party::kLastHeroId)
        {
            return 0;
        }
        const uintptr_t o = World::objectAddress(id);
        uint32_t type = 0;
        return o && readAt(o, kType, type) && type >= 1 && type <= 9 && World::heroClass(id) ? o : 0;
    }

    bool sameOwnerAsLocal(uintptr_t o)
    {
        uint8_t owner = 0;
        return readAt(o, kOwner, owner) && owner == World::localId();
    }

    double distance(uintptr_t a, uintptr_t b)
    {
        int32_t pa[2] = {}, pb[2] = {};
        if (!readAt(a, kPos + 4, pa) || !readAt(b, kPos + 4, pb))
        {
            return 1e18;
        }
        return std::hypot(static_cast<double>(pa[0]) - pb[0], static_cast<double>(pa[1]) - pb[1]);
    }

    // Rewrites the owner byte of every object from `from` to `to`; the count, -1 on a fault.
    int reown(uint8_t from, uint8_t to)
    {
        uintptr_t *begin = nullptr, *end = nullptr;
        if (!objectRange(begin, end))
        {
            return -1;
        }
        int n = 0;
        __try
        {
            for (uintptr_t* p = begin; p < end; ++p)
            {
                uint8_t* o = reinterpret_cast<uint8_t*>(*p);
                if (o && o[kOwner] == from)
                {
                    o[kOwner] = to;
                    ++n;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return n;
    }

    // The anti-cheat shadows of the local hero (ENG 0x590730 checks hero()'s experience, gold, level and stats words
    // against them and resets what differs to 1): two pages, the first kept read-protected, each [0] experience,
    // [1] gold, [2] level XOR a key (the hero's byte +0x38 four times; doubled for the second page), then a copy of
    // the creature's +0x3B0 (0x3B dwords and a word). Written as the engine writes them after a legitimate change
    // (the end of 0x55E1B0, 0x57E160, 0x580110).
    bool refreshShadow(uintptr_t hero)
    {
        if (!Addr::g_antiCheatShadow1 || !Addr::g_antiCheatShadow2)
        {
            return false;
        }
        uint32_t* s1 = nullptr;
        uint32_t* s2 = nullptr;
        if (!safeCopy(&s1, Addr::g_antiCheatShadow1.get(), sizeof(s1)) || !safeCopy(&s2, Addr::g_antiCheatShadow2.get(), sizeof(s2)) ||
            !s1 || !s2)
        {
            return false;
        }
        uint8_t keyByte = 0;
        uint32_t experience = 0, gold = 0;
        uint16_t level = 0;
        uint8_t words[0x3B * 4 + 2] = {};
        if (!readAt(hero, 0x38, keyByte) || !readAt(hero, kExperience, experience) || !readAt(hero, kGold, gold) ||
            !readAt(hero, kLevel, level) || !safeCopy(words, reinterpret_cast<const void*>(hero + kShadowCopy), sizeof(words)))
        {
            return false;
        }
        const uint32_t key = keyByte * 0x01010101u;
        DWORD old = 0;
        if (!VirtualProtect(s1, 0x1000, PAGE_READWRITE, &old))
        {
            return false;
        }
        bool ok = true;
        for (int page = 0; page < 2; ++page)
        {
            uint32_t* s = page ? s2 : s1;
            const uint32_t k = page ? key << 1 : key;
            const uint32_t head[3] = {experience ^ k, gold ^ k, static_cast<uint32_t>(level) ^ k};
            ok = safeCopy(s, head, sizeof(head)) && safeCopy(s + 3, words, sizeof(words)) && ok;
        }
        DWORD ignored = 0;
        VirtualProtect(s1, 0x1000, old, &ignored);
        return ok;
    }

    // Moves the mercenaries (the follower vector, whose members follow and fight for its owner) from one hero to
    // another and tells them their new leader.
    void moveFollowers(uintptr_t from, uintptr_t to, uint32_t toId)
    {
        uintptr_t a[3] = {}, b[3] = {};
        if (!readAt(from, kFollowers, a) || !readAt(to, kFollowers, b))
        {
            return;
        }
        uint32_t fromId = 0;
        readAt(from, kId, fromId);
        // Swap the vectors (the same allocator made both), then point the members at their new leader.
        writeAt(from, kFollowers, b);
        writeAt(to, kFollowers, a);
        const uintptr_t begin = a[0], end = a[1];
        for (uintptr_t e = begin; e && e + 9 <= end; e += 9)
        {
            uint32_t id = 0;
            int32_t master = 0;
            const uintptr_t m = readAt(e, 0, id) ? World::objectAddress(id) : 0;
            if (m && readAt(m, kMaster, master) && static_cast<uint32_t>(master) == fromId)
            {
                writeAt(m, kMaster, static_cast<int32_t>(toId));
            }
        }
    }

    bool deliver(uintptr_t creature, const uint32_t (&event)[0x11])
    {
        __try
        {
            using Handler = uint32_t(__fastcall*)(uintptr_t self, void* edx, const void* event);
            const Handler h = (*reinterpret_cast<Handler* const*>(creature))[6];
            h(creature, nullptr, event);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool callAddExperience(uintptr_t stats, uint32_t amount);

    decltype(Addr::cStats_addExperience)::Ptr g_origAddExperience = nullptr;
    double g_share = 0;
    double g_shareRadius = 0;
    bool g_sharing = false;

    // The engine's experience gain (kills, quests): the party shares it.
    void __fastcall hookAddExperience(void* stats, void* edx, uint32_t amount)
    {
        g_origAddExperience(stats, edx, amount);
        if (g_sharing || g_share <= 0 || amount == 0 || amount >= 0x7FFFFFFF || World::networkGame())
        {
            return;
        }
        uintptr_t gainer = 0;
        uint32_t gainerId = 0;
        if (!readAt(reinterpret_cast<uintptr_t>(stats), 4, gainer) || !readAt(gainer, kId, gainerId) || !heroObject(gainerId))
        {
            return;
        }
        const uint32_t part = static_cast<uint32_t>(std::min(amount * g_share, 2.0e9));
        if (part == 0)
        {
            return;
        }
        g_sharing = true;
        for (uint32_t id : Party::heroes())
        {
            const uintptr_t o = heroObject(id);
            int32_t health = 0;
            if (id == gainerId || !o || !sameOwnerAsLocal(o) || !readAt(o, kHealth, health) || health <= 0 ||
                (g_shareRadius > 0 && distance(o, gainer) > g_shareRadius))
            {
                continue;
            }
            callAddExperience(o + kStats, part);
        }
        g_sharing = false;
    }

    bool callAddExperience(uintptr_t stats, uint32_t amount)
    {
        const auto fn = g_origAddExperience ? g_origAddExperience : Addr::cStats_addExperience.ptr();
        __try
        {
            fn(reinterpret_cast<void*>(stats), nullptr, amount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool callInitSpells(uintptr_t stats)
    {
        __try
        {
            Addr::cStats_initSpells(reinterpret_cast<void*>(stats), 0, -1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int callIsEnemy(uintptr_t a, uintptr_t b)
    {
        __try
        {
            return Addr::cCreature_isEnemy(reinterpret_cast<void*>(a), reinterpret_cast<void*>(b)) & 0xFF ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The engine takes the buffer (its allocator: engine_alloc) and frees it.
    bool callSetText(uint32_t textId, const wchar_t* text, int length)
    {
        __try
        {
            Addr::textResources_instance();
            const uint32_t bytes = static_cast<uint32_t>((length + 1) * 2);
            auto* buffer = static_cast<wchar_t*>(Addr::engine_alloc(bytes));
            if (!buffer)
            {
                return false;
            }
            std::memcpy(buffer, text, length * 2);
            buffer[length] = 0;
            using SetFn = void(__stdcall*)(uint32_t id, wchar_t* begin, wchar_t* end, wchar_t* capacity);
            reinterpret_cast<SetFn>(Addr::textResources_set)(textId, buffer, buffer + length, buffer + length + 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int callPortrait(uint32_t type)
    {
        __try
        {
            return static_cast<int>(reinterpret_cast<uint32_t(__stdcall*)(uint32_t)>(Addr::creaturePortraitImage)(type));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

void Party::install()
{
    if (!Addr::cStats_addExperience)
    {
        LOG("Party: the experience function was not found: no shared experience");
        return;
    }
    Patch::hook(g_origAddExperience, Addr::cStats_addExperience, hookAddExperience, "cStats_addExperience");
}

void Party::frame()
{
    // A savegame load sets the local id back to 1 but leaves the engine's mouse target on the hero that was controlled.
    if (World::networkGame())
    {
        return;
    }
    const uintptr_t e = engine();
    const uint32_t local = World::localId();
    uint32_t controlled = 0;
    if (e && local && readAt(e, kEngineControlled, controlled) && controlled >= 1 && controlled <= kLastHeroId &&
        controlled != local && heroObject(local))
    {
        writeAt(e, kEngineControlled, local);
    }
}

bool Party::available(const char*& why)
{
    if (!World::available() || !World::hero())
    {
        why = "no game is running";
        return false;
    }
    if (World::networkGame())
    {
        why = "the party works in single player only";
        return false;
    }
    if (!engine() || !players())
    {
        why = "the game's engine objects were not found";
        return false;
    }
    return true;
}

std::vector<uint32_t> Party::heroes()
{
    std::vector<uint32_t> out;
    const uint32_t local = World::localId();
    if (heroObject(local))
    {
        out.push_back(local);
    }
    for (uint32_t id = 1; id <= kLastHeroId; ++id)
    {
        if (id != local && heroObject(id))
        {
            out.push_back(id);
        }
    }
    return out;
}

bool Party::isHero(uint32_t id)
{
    return heroObject(id) != 0;
}

uint32_t Party::recruit(uint32_t heroClass, const World::Position& at, const char*& error)
{
    if (!available(error))
    {
        return 0;
    }
    if (heroClass < 1 || heroClass > 9 || heroClass == 7 || !World::classBit(heroClass))
    {
        error = "no such hero class";
        return 0;
    }
    uint32_t id = 0;
    for (uint32_t i = kFirstCompanionId; i <= kLastHeroId && !id; ++i)
    {
        if (!World::objectAddress(i) && Inventory::items(i).empty())
        {
            id = i;
        }
    }
    if (!id)
    {
        error = "the party is full";
        return 0;
    }
    const uint32_t made = World::createObject(heroClass, at, false, error, id);
    const uintptr_t o = made == id ? World::objectAddress(id) : 0;
    if (!o)
    {
        if (made)
        {
            World::destroyObject(made);
        }
        if (!error || !*error)
        {
            error = "the game did not create the hero";
        }
        return 0;
    }
    // What the engine's own hero creation (ENG 0x60D9F0) does after creating the hero: its home is where it stands,
    // and its stats block finishes setting up.
    uint8_t pos[13] = {};
    if (safeCopy(pos, reinterpret_cast<const void*>(o + kPos), sizeof(pos)))
    {
        safeCopy(reinterpret_cast<void*>(o + kHome), pos, sizeof(pos));
    }
    callInitSpells(o + kStats);
    LOG("Party: recruited a hero of class {} as id {}", heroClass, id);
    return id;
}

bool Party::dismiss(uint32_t id, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    if (!heroObject(id) || id < kFirstCompanionId)
    {
        error = "not a companion";
        return false;
    }
    if (id == World::localId())
    {
        error = "the controlled hero cannot be dismissed";
        return false;
    }
    if (!Inventory::items(id).empty())
    {
        error = "the companion still carries items in its backpack";
        return false;
    }
    if (!World::destroyObject(id))
    {
        error = "the game did not remove the hero";
        return false;
    }
    LOG("Party: dismissed hero {}", id);
    return true;
}

bool Party::control(uint32_t id, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uint32_t old = World::localId();
    const uintptr_t from = heroObject(old);
    const uintptr_t to = heroObject(id);
    if (!to)
    {
        error = "not a party hero";
        return false;
    }
    if (id == old)
    {
        return true;
    }
    if (!from)
    {
        error = "the controlled hero was not found";
        return false;
    }
    int32_t health = 0;
    if (!readAt(to, kHealth, health) || health <= 0)
    {
        error = "the hero is dead";
        return false;
    }
    uint32_t flags = 0;
    if (!readAt(to, kFlags, flags) || (flags & kFlagGone))
    {
        error = "the hero is not in the world";
        return false;
    }
    const uintptr_t p = players();
    const uintptr_t e = engine();
    // The gold is the party's: it goes with control.
    uint32_t goldFrom = 0, goldTo = 0;
    if (readAt(from, kGold, goldFrom) && readAt(to, kGold, goldTo))
    {
        const uint32_t sum = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(goldFrom) + goldTo, 0x7FFFFFFE));
        writeAt(to, kGold, sum);
        writeAt(from, kGold, uint32_t{0});
    }
    const int moved = reown(static_cast<uint8_t>(old), static_cast<uint8_t>(id));
    if (moved < 0 || !writeAt(p, kPlayersLocal, id))
    {
        reown(static_cast<uint8_t>(id), static_cast<uint8_t>(old));
        error = "cannot write the players object";
        return false;
    }
    uint32_t controlled = 0;
    if (readAt(e, kEngineControlled, controlled) && (controlled == old || controlled <= kLastHeroId))
    {
        writeAt(e, kEngineControlled, id);
    }
    moveFollowers(from, to, id);
    if (!refreshShadow(to))
    {
        LOG("Party: the anti-cheat shadow could not be refreshed");
    }
    LOG("Party: control {} -> {} ({} objects change owner)", old, id, moved);
    return true;
}

bool Party::order(uint32_t id, const Order& o, const char*& error)
{
    const char* why = nullptr;
    if (!available(why))
    {
        error = why;
        return false;
    }
    const uintptr_t c = heroObject(id);
    if (!c || !sameOwnerAsLocal(c))
    {
        error = "not a party hero";
        return false;
    }
    if (!Addr::g_creatureEventVtable)
    {
        error = "the game's order class was not found";
        return false;
    }
    uint32_t ev[0x11] = {};
    ev[0] = static_cast<uint32_t>(Addr::g_creatureEventVtable.address);
    auto targetPos = [&](uint32_t target, World::Position& out) {
        World::Object t;
        if (!World::object(target, t) || !t.creature)
        {
            return false;
        }
        out = t.position;
        return true;
    };
    switch (o.kind)
    {
    case OrderKind::Move:
        ev[1] = kOrderMove;
        ev[5] = 2;      // walk
        ev[6] = static_cast<uint32_t>(o.at.x);
        ev[7] = static_cast<uint32_t>(o.at.y);
        break;
    case OrderKind::Stop:
    {
        World::Object self;
        if (!World::object(id, self))
        {
            error = "not a party hero";
            return false;
        }
        ev[1] = kOrderMove;
        ev[5] = 0;      // stop
        ev[6] = static_cast<uint32_t>(self.position.x);
        ev[7] = static_cast<uint32_t>(self.position.y);
        break;
    }
    case OrderKind::Attack:
    {
        World::Position at;
        if (!targetPos(o.target, at))
        {
            error = "no such creature";
            return false;
        }
        ev[1] = kOrderAttack;
        ev[6] = o.target;
        ev[7] = static_cast<uint32_t>(at.x);
        ev[8] = static_cast<uint32_t>(at.y);
        break;
    }
    case OrderKind::Ability:
    {
        World::Position at = o.at;
        if (o.target && !targetPos(o.target, at))
        {
            error = "no such creature";
            return false;
        }
        std::vector<Ability> known;
        abilities(id, known);
        if (std::none_of(known.begin(), known.end(), [&](const Ability& a) { return a.id == o.ability; }))
        {
            error = "the hero does not know that combat art or spell";
            return false;
        }
        ev[1] = kOrderAbility;
        ev[4] = o.target;
        ev[5] = at.sector;
        ev[6] = static_cast<uint32_t>(at.x);
        ev[7] = static_cast<uint32_t>(at.y);
        ev[10] = 100;
        ev[11] = o.ability >= 1000 ? 1 : 0;    // a combat art (ids from 1000) or a spell
        ev[12] = o.ability;
        ev[13] = at.layer;
        break;
    }
    }
    if (!deliver(c, ev))
    {
        error = "the game faulted on the order";
        return false;
    }
    return true;
}

bool Party::abilities(uint32_t id, std::vector<Ability>& out)
{
    out.clear();
    const uintptr_t c = heroObject(id);
    uintptr_t range[2] = {};
    if (!c || !readAt(c, kAbilities, range) || !range[0] || range[1] < range[0] || range[1] - range[0] > kAbilitySize * 512)
    {
        return false;
    }
    for (uintptr_t e = range[0]; e + kAbilitySize <= range[1]; e += kAbilitySize)
    {
        Ability a;
        if (readAt(e, kAbilityId, a.id) && readAt(e, kAbilityLevel, a.level) && readAt(e, kAbilityBonus, a.bonus) &&
            readAt(e, kAbilityRegenMax, a.regenMax) && readAt(e, kAbilityRegen, a.regen) && a.id)
        {
            if (!std::isfinite(a.regen) || a.regen < 0)
            {
                a.regen = 0;
            }
            if (!std::isfinite(a.regenMax) || a.regenMax < 0)
            {
                a.regenMax = 0;
            }
            out.push_back(a);
        }
    }
    return true;
}

bool Party::state(uint32_t id, State& out)
{
    const uintptr_t c = heroObject(id);
    return c && readAt(c, kAction, out.action) && readAt(c, kMovement, out.movement);
}

bool Party::addExperience(uint32_t id, uint32_t amount, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uintptr_t c = heroObject(id);
    if (!c || !sameOwnerAsLocal(c))
    {
        error = "not a party hero";
        return false;
    }
    if (!Addr::cStats_addExperience)
    {
        error = "the game's experience function was not found";
        return false;
    }
    const bool was = g_sharing;
    g_sharing = true;   // what a mod gives is not shared
    const bool ok = callAddExperience(c + kStats, amount);
    g_sharing = was;
    if (!ok)
    {
        error = "the game faulted while adding experience";
    }
    return ok;
}

int Party::hostile(uint32_t id, uint32_t other)
{
    const uintptr_t a = World::objectAddress(id);
    const uintptr_t b = World::objectAddress(other);
    World::Object oa, ob;
    if (!a || !b || !Addr::cCreature_isEnemy || !World::object(id, oa) || !World::object(other, ob) || !oa.creature ||
        !ob.creature)
    {
        return -1;
    }
    return callIsEnemy(a, b);
}

bool Party::setName(uint32_t id, const std::string& name, const char*& error)
{
    if (!heroObject(id))
    {
        error = "not a hero";
        return false;
    }
    if (!Addr::textResources_set || !Addr::textResources_instance || !Addr::engine_alloc)
    {
        error = "the game's text functions were not found";
        return false;
    }
    wchar_t wide[32] = {};
    const int n = name.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide, 31);
    if (n < 1 || n > 30)
    {
        error = "the name must be 1 to 30 characters";
        return false;
    }
    if (!callSetText(0x26FF + id, wide, n))
    {
        error = "the game faulted while setting the name";
        return false;
    }
    return true;
}

int Party::portraitImage(uint32_t type)
{
    if (!Addr::creaturePortraitImage)
    {
        return -1;
    }
    const int r = callPortrait(type);
    return r > 0 ? r : -1;
}

void Party::setExperienceShare(double share, double radius)
{
    g_share = std::clamp(share, 0.0, 10.0);
    g_shareRadius = std::max(radius, 0.0);
}
