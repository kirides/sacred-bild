#include "classes.h"
#include "log.h"
#include "lua_host.h"
#include "world.h"
#include "game/gear_addr.h"
#include "patch.h"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace
{
    using namespace Gear;

    constexpr uintptr_t kType = 0x10, kArtInFlight = 0x120, kAction = 0x150, kStats = 0x3A8;
    constexpr uintptr_t kAbilities = kStats + 0x102;                    // vector {begin, end, capacity} of 0x16-byte entries
    constexpr uintptr_t kAbilityKind = 0, kAbilityId = 4, kAbilityLevel = 6, kAbilitySize = 0x16;
    constexpr uintptr_t kModels = 0x48, kModelsEnd = 0x4C;              // the model manager's model records (0x4AA bytes)
    constexpr uintptr_t kModelSize = 0x4AA, kModelSlots = 0x70;
    constexpr uintptr_t kTypeNames = 0x37, kTypeSize = 0x80;            // the type manager's 0x80-byte records, the model name at +0x37

    std::atomic<bool> g_routeActive{false}, g_motionActive{false};
    std::atomic<unsigned> g_routeCalls{0}, g_motionCalls{0};

    decltype(Addr::cCreature_classOf)::Ptr g_origClassOf = nullptr;
    decltype(Addr::cCreature_playMotion)::Ptr g_origPlayMotion = nullptr;

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool isHeroType(uint32_t type)
    {
        return type >= 1 && type <= 9;
    }

    // The class (1..9, what classOf returns) that runs the art in flight, from the mods; the engine's own where none answers.
    int __fastcall hookClassOf(void* creature, void* edx)
    {
        const int vanilla = g_origClassOf(creature, edx);
        if (!g_routeActive.load(std::memory_order_relaxed) || !creature)
        {
            return vanilla;
        }
        uint32_t type = 0;
        uint16_t art = 0;
        if (!readAt(reinterpret_cast<uintptr_t>(creature), kType, type) || !isHeroType(type) ||
            !readAt(reinterpret_cast<uintptr_t>(creature), kArtInFlight, art) || art < 1000)
        {
            return vanilla;
        }
        uint16_t action = 0;
        readAt(reinterpret_cast<uintptr_t>(creature), kAction, action);
        g_routeCalls.fetch_add(1, std::memory_order_relaxed);
        int answer = 0;
        uint32_t id = 0;
        readAt(reinterpret_cast<uintptr_t>(creature), 0x0C, id);
        if (LuaHost::abilityRoute(static_cast<int>(type), art, vanilla, action, id, answer) == LuaHost::Answer::Value && answer >= 1 && answer <= 9)
        {
            return answer;
        }
        return vanilla;
    }

    void __fastcall hookPlayMotion(void* creature, void* edx, int slot, float a, float b, float c)
    {
        if (g_motionActive.load(std::memory_order_relaxed) && creature)
        {
            uint32_t type = 0, id = 0;
            if (readAt(reinterpret_cast<uintptr_t>(creature), kType, type) && isHeroType(type))
            {
                g_motionCalls.fetch_add(1, std::memory_order_relaxed);
                readAt(reinterpret_cast<uintptr_t>(creature), 0x0C, id);
                int answer = 0;
                if (LuaHost::motionSelect(static_cast<int>(type), slot, id, answer) == LuaHost::Answer::Value && answer >= 0 && answer < 256)
                {
                    slot = answer;
                }
            }
        }
        g_origPlayMotion(creature, edx, slot, a, b, c);
    }

    // The hero object for id 1..16, a live creature with a hero type.
    uintptr_t heroObject(uint32_t id)
    {
        if (id < 1 || id > 16)
        {
            return 0;
        }
        const uintptr_t o = World::objectAddress(id);
        uint32_t type = 0;
        return o && readAt(o, kType, type) && isHeroType(type) ? o : 0;
    }

    struct Range
    {
        uintptr_t begin = 0, end = 0, capacity = 0;
    };

    bool abilityRange(uintptr_t hero, Range& range)
    {
        return readAt(hero, kAbilities, range) && range.begin && range.end >= range.begin &&
               range.end - range.begin <= kAbilitySize * 1024 && (range.end - range.begin) % kAbilitySize == 0;
    }

    // The engine's learn function takes the 0x16-byte entry by value (six dwords on the stack).
    using Entry = Gear::Addr::LearnEntry;

    // The model record of a creature type: found by the model name in the type record (compared without case).
    uintptr_t modelRecord(uint32_t type)
    {
        if (!Addr::g_pTypeManager || !Addr::g_pModelManager)
        {
            return 0;
        }
        const uintptr_t types = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(Addr::g_pTypeManager.address));
        const uintptr_t models = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(Addr::g_pModelManager.address));
        if (!types || !models || type == 0 || type >= 0x7E60)
        {
            return 0;
        }
        char name[0x80] = {};
        __try
        {
            const char* source = reinterpret_cast<const char*>(types + kTypeNames + type * kTypeSize);
            strncpy_s(name, source, 0x3F);
            name[0x3F] = 0;
            uintptr_t begin = 0, end = 0;
            std::memcpy(&begin, reinterpret_cast<const void*>(models + kModels), 4);
            std::memcpy(&end, reinterpret_cast<const void*>(models + kModelsEnd), 4);
            if (!begin || end <= begin || !name[0])
            {
                return 0;
            }
            for (uintptr_t r = begin; r + kModelSize <= end; r += kModelSize)
            {
                if (_stricmp(reinterpret_cast<const char*>(r), name) == 0)
                {
                    return r;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return 0;
    }
}

bool Classes::install()
{
    if (!Addr::cCreature_classOf || !Addr::cCreature_playMotion)
    {
        LOG("Classes: the class and motion code was not found: no ability.route / motion.select");
        return false;
    }
    bool ok = Patch::hook(g_origClassOf, Addr::cCreature_classOf, hookClassOf, "cCreature_classOf");
    ok = Patch::hook(g_origPlayMotion, Addr::cCreature_playMotion, hookPlayMotion, "cCreature_playMotion") && ok;
    ok = installAliases() && ok;
    return ok;
}

void Classes::setRouteActive(bool active)
{
    g_routeActive.store(active);
}

void Classes::setMotionActive(bool active)
{
    g_motionActive.store(active);
}

bool Classes::learn(uint32_t id, uint32_t ability, uint32_t level, const char*& error)
{
    const uintptr_t hero = heroObject(id);
    if (!hero)
    {
        error = "not a hero";
        return false;
    }
    if (!Addr::cStats_learnAbility)
    {
        error = "the game's learn function was not found";
        return false;
    }
    if (ability == 0 || ability > 0xFFFF)
    {
        error = "an ability id is 1..65535";
        return false;
    }
    Range range;
    if (!abilityRange(hero, range))
    {
        error = "the hero has no abilities vector";
        return false;
    }
    for (uintptr_t e = range.begin; e + kAbilitySize <= range.end; e += kAbilitySize)
    {
        uint16_t known = 0;
        if (readAt(e, kAbilityId, known) && known == ability)
        {
            error = "the hero knows it already";
            return false;
        }
    }
    Entry entry = {};
    const uint32_t kind = ability >= 1000 ? 1 : 2;
    std::memcpy(entry.bytes + kAbilityKind, &kind, 4);
    const uint16_t shortId = static_cast<uint16_t>(ability);
    std::memcpy(entry.bytes + kAbilityId, &shortId, 2);
    entry.bytes[kAbilityLevel] = static_cast<uint8_t>(level ? level : 1);
    __try
    {
        Addr::cStats_learnAbility(reinterpret_cast<void*>(hero + kStats), entry);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "the game faulted on the learn function";
        return false;
    }
    return true;
}

bool Classes::forget(uint32_t id, uint32_t ability, const char*& error)
{
    const uintptr_t hero = heroObject(id);
    if (!hero)
    {
        error = "not a hero";
        return false;
    }
    Range range;
    if (!abilityRange(hero, range))
    {
        error = "the hero has no abilities vector";
        return false;
    }
    __try
    {
        for (uintptr_t e = range.begin; e + kAbilitySize <= range.end; e += kAbilitySize)
        {
            uint16_t known = 0;
            std::memcpy(&known, reinterpret_cast<const void*>(e + kAbilityId), 2);
            if (known == ability)
            {
                std::memmove(reinterpret_cast<void*>(e), reinterpret_cast<const void*>(e + kAbilitySize), range.end - e - kAbilitySize);
                const uintptr_t end = range.end - kAbilitySize;
                std::memcpy(reinterpret_cast<void*>(hero + kAbilities + 4), &end, 4);
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "the game's memory faulted";
        return false;
    }
    error = "the hero does not know it";
    return false;
}

int Classes::motionRecord(uint32_t type, uint32_t slot)
{
    const uintptr_t model = modelRecord(type);
    uint32_t record = 0;
    if (!model || slot > 255 || !readAt(model, kModelSlots + slot * 4, record))
    {
        return -1;
    }
    return static_cast<int>(record);
}

bool Classes::setMotionRecord(uint32_t type, uint32_t slot, uint32_t record)
{
    const uintptr_t model = modelRecord(type);
    if (!model || slot > 255)
    {
        return false;
    }
    __try
    {
        std::memcpy(reinterpret_cast<void*>(model + kModelSlots + slot * 4), &record, 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int Classes::hasMotion(uint32_t type, uint32_t slot)
{
    const int record = motionRecord(type, slot);
    if (record < 0)
    {
        return -1;
    }
    return record != 0 ? 1 : 0;
}
