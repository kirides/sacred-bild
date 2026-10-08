#include "game/aim_assist.h"
#include "game/frame_hooks.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "log.h"
#include "mem.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
    using namespace Sacred;

    // thiscall targets are called and hooked as fastcall with an unused EDX parameter.
    using PickFn = uint32_t(__fastcall*)(void* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect);
    using GetDataFn = void*(__fastcall*)(void* manager, void* edx, uint32_t id);
    using HeroFn = void*(__fastcall*)(void* manager, void* edx);
    using DynamicCastFn = void*(__cdecl*)(void* object, long vfDelta, void* source, void* target, int isReference);
    using IsEnemyFn = uint32_t(__fastcall*)(void* creature, void* edx, void* target);

    PickFn g_origPick = nullptr;

    enum PickMode : int
    {
        PickGame,
        PickNothing,
        PickTarget,
    };
    std::atomic<int> g_mode{PickGame};
    std::atomic<uint32_t> g_target{0};

    constexpr size_t kMaxEntries = 4096;
    constexpr float kNotInCone = 1e6f;

    struct Entry
    {
        uint32_t id;
        int32_t x, y;
        int16_t w, h;

        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
    };

    using Mem::member;

    // The world view the cursor picks in (the engine's current view), nullptr before a game ran.
    void* worldView()
    {
        void* engine = FrameHooks::engine();
        if (!engine)
        {
            return nullptr;
        }
        const uint16_t index = member<uint16_t>(engine, Engine::viewIndex);
        return index < 8 ? member<void*>(engine, Engine::views + index * 4) : nullptr;
    }

    // Calls f(entry) for the view's pick list under its lock, until f returns false.
    template <class F>
    void scan(void* view, F&& f)
    {
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uint8_t*>(view) + WorldView::pickLock);
        EnterCriticalSection(lock);
        const auto* p = member<const uint8_t*>(view, WorldView::pickBegin);
        const auto* end = member<const uint8_t*>(view, WorldView::pickEnd);
        if (p && end >= p && static_cast<size_t>(end - p) / PickEntry::size <= kMaxEntries)
        {
            for (; p + PickEntry::size <= end; p += PickEntry::size)
            {
                Entry e;
                std::memcpy(&e.id, p + PickEntry::id, 4);
                std::memcpy(&e.x, p + PickEntry::x, 4);
                std::memcpy(&e.y, p + PickEntry::y, 4);
                std::memcpy(&e.w, p + PickEntry::width, 2);
                std::memcpy(&e.h, p + PickEntry::height, 2);
                if (!f(e))
                {
                    break;
                }
            }
        }
        LeaveCriticalSection(lock);
    }

    bool findEntry(void* view, uint32_t id, Entry& out)
    {
        bool found = false;
        scan(view, [&](const Entry& e) {
            if (e.id != id)
            {
                return true;
            }
            out = e;
            found = true;
            return false;
        });
        return found;
    }

    void* objectManager()
    {
        return *reinterpret_cast<void**>(Addr::g_pObjectManager);
    }

    void* creatureOf(uint32_t id)
    {
        void* manager = objectManager();
        if (!manager || (id & 0x80000000u))
        {
            return nullptr;
        }
        void* object = reinterpret_cast<GetDataFn>(Addr::cObjectManager_getData)(manager, nullptr, id);
        if (!object)
        {
            return nullptr;
        }
        return reinterpret_cast<DynamicCastFn>(Addr::rtDynamicCast)(object, 0,
            reinterpret_cast<void*>(Addr::cObject_typeDescriptor), reinterpret_cast<void*>(Addr::cCreature_typeDescriptor), 0);
    }

    void* heroCreature()
    {
        void* manager = objectManager();
        return manager ? reinterpret_cast<HeroFn>(Addr::cObjectManager_hero)(manager, nullptr) : nullptr;
    }

    bool isEnemy(void* hero, void* creature)
    {
        return hero && creature && (reinterpret_cast<IsEnemyFn>(Addr::cCreature_isEnemy)(hero, nullptr, creature) & 0xFF);
    }

    uint32_t __fastcall hookPick(void* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect)
    {
        switch (g_mode.load(std::memory_order_relaxed))
        {
        case PickNothing:
            return 0;
        case PickTarget:
        {
            const uint32_t id = g_target.load(std::memory_order_relaxed);
            Entry e;
            if (id && id != exclude && findEntry(view, id, e))
            {
                if (rect)
                {
                    rect[0] = e.x;
                    rect[1] = e.y;
                    rect[2] = static_cast<uint16_t>(e.w) | (static_cast<int32_t>(e.h) << 16);
                }
                return id;
            }
            break;
        }
        default:
            break;
        }
        return g_origPick(view, edx, x, y, exclude, rect);
    }

    // Lower is better: inside the cone by distance (a little more toward its edge), then the rest by distance.
    float score(float vx, float vy, float dirX, float dirY, float cone)
    {
        const float distance = std::sqrt(vx * vx + vy * vy);
        const float dirLength = std::sqrt(dirX * dirX + dirY * dirY);
        if (dirLength < 1e-3f || distance < 1.0f)
        {
            return distance;
        }
        const float cosine = std::clamp((vx * dirX + vy * dirY) / (distance * dirLength), -1.0f, 1.0f);
        const float angle = std::acos(cosine) * 57.29578f;
        const float half = std::max(1.0f, cone * 0.5f);
        return angle <= half ? distance * (1.0f + 0.5f * angle / half) : kNotInCone + distance;
    }
}

void AimAssist::install()
{
    Patch::hook(g_origPick, Addr::worldPick, &hookPick, "worldPick");
}

bool AimAssist::heroHealth(int& health, int& maximum)
{
    void* hero = heroCreature();
    if (!hero)
    {
        return false;
    }
    health = member<int32_t>(hero, Creature::health);
    maximum = member<int32_t>(hero, Creature::maxHealth);
    return true;
}

bool AimAssist::hero(float& x, float& y)
{
    x = Resolution::width() * 0.5f;
    y = Resolution::height() * 0.5f;
    void* view = worldView();
    void* hero = heroCreature();
    if (!view || !hero)
    {
        return false;
    }
    Entry e;
    if (findEntry(view, member<uint32_t>(hero, Object::id), e))
    {
        x = e.cx();
        y = e.cy();
    }
    return true;
}

bool AimAssist::find(Kind kind, float dirX, float dirY, float range, float cone, Target& out)
{
    void* view = worldView();
    void* hero = heroCreature();
    float hx, hy;
    if (!view || !hero || !AimAssist::hero(hx, hy))
    {
        return false;
    }
    const uint32_t heroId = member<uint32_t>(hero, Object::id);
    float best = 0.0f;
    bool found = false;
    scan(view, [&](const Entry& e) {
        if (e.id == 0 || e.id == heroId || (e.id & 0x80000000u))
        {
            return true;
        }
        const float vx = e.cx() - hx, vy = e.cy() - hy;
        if (vx * vx + vy * vy > range * range)
        {
            return true;
        }
        const bool enemy = isEnemy(hero, creatureOf(e.id));
        if (enemy != (kind == Kind::Enemy))
        {
            return true;
        }
        const float s = score(vx, vy, dirX, dirY, cone);
        if (!found || s < best)
        {
            best = s;
            out = {e.id, e.cx(), e.cy()};
            found = true;
        }
        return true;
    });
    return found;
}

bool AimAssist::locate(uint32_t id, Kind kind, Target& out)
{
    void* view = worldView();
    Entry e;
    if (!view || !id || !findEntry(view, id, e))
    {
        return false;
    }
    if (kind == Kind::Enemy && !isEnemy(heroCreature(), creatureOf(id)))
    {
        return false;
    }
    out = {id, e.cx(), e.cy()};
    return true;
}

void AimAssist::pickGame()
{
    g_mode = PickGame;
}

void AimAssist::pickNothing()
{
    g_mode = PickNothing;
}

void AimAssist::pickTarget(uint32_t id)
{
    g_target = id;
    g_mode = PickTarget;
}
