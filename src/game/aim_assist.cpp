#include "game/aim_assist.h"
#include "game/frame_hooks.h"
#include "game/resolution.h"
#include "log.h"
#include "patch.h"
#include "sacred/engine.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
    using namespace Sacred;

    decltype(Addr::worldPick)::Ptr g_origPick = nullptr;

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

    // The world view the cursor picks in (the engine's current view), nullptr before a game ran.
    cWorldView* worldView()
    {
        cEngine* engine = FrameHooks::engine();
        return engine ? engine->worldView() : nullptr;
    }

    // Calls f(entry) for the view's pick list under its lock, until f returns false.
    template <class F>
    void scan(cWorldView* view, F&& f)
    {
        EnterCriticalSection(&view->pickLock);
        const PickEntry* p = view->pickBegin;
        const PickEntry* end = view->pickEnd;
        if (p && end >= p && static_cast<size_t>(end - p) <= kMaxEntries)
        {
            for (; p < end; ++p)
            {
                if (!f(*p))
                {
                    break;
                }
            }
        }
        LeaveCriticalSection(&view->pickLock);
    }

    bool findEntry(cWorldView* view, uint32_t id, PickEntry& out)
    {
        bool found = false;
        scan(view, [&](const PickEntry& e) {
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

    cCreature* creatureOf(uint32_t id)
    {
        cObjectManager* manager = cObjectManager::instance();
        return manager ? manager->creature(id) : nullptr;
    }

    cCreature* heroCreature()
    {
        cObjectManager* manager = cObjectManager::instance();
        return manager ? manager->hero() : nullptr;
    }

    bool isEnemy(cCreature* hero, cCreature* creature)
    {
        return hero && creature && hero->isEnemy(creature);
    }

    uint32_t __fastcall hookPick(cWorldView* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect)
    {
        switch (g_mode.load(std::memory_order_relaxed))
        {
        case PickNothing:
            return 0;
        case PickTarget:
        {
            const uint32_t id = g_target.load(std::memory_order_relaxed);
            PickEntry e;
            if (id && id != exclude && findEntry(view, id, e))
            {
                if (rect)
                {
                    rect[0] = e.x;
                    rect[1] = e.y;
                    rect[2] = static_cast<uint16_t>(e.width) | (static_cast<int32_t>(e.height) << 16);
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
    cCreature* hero = heroCreature();
    if (!hero)
    {
        return false;
    }
    health = hero->health;
    maximum = hero->maxHealth;
    return true;
}

bool AimAssist::hero(float& x, float& y)
{
    x = Resolution::width() * 0.5f;
    y = Resolution::height() * 0.5f;
    cWorldView* view = worldView();
    cCreature* hero = heroCreature();
    if (!view || !hero)
    {
        return false;
    }
    PickEntry e;
    if (findEntry(view, hero->id, e))
    {
        x = e.centerX();
        y = e.centerY();
    }
    return true;
}

bool AimAssist::find(Kind kind, float dirX, float dirY, float range, float cone, Target& out)
{
    cWorldView* view = worldView();
    cCreature* hero = heroCreature();
    float hx, hy;
    if (!view || !hero || !AimAssist::hero(hx, hy))
    {
        return false;
    }
    const uint32_t heroId = hero->id;
    float best = 0.0f;
    bool found = false;
    scan(view, [&](const PickEntry& e) {
        if (e.id == 0 || e.id == heroId || (e.id & 0x80000000u))
        {
            return true;
        }
        const float vx = e.centerX() - hx, vy = e.centerY() - hy;
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
            out = {e.id, e.centerX(), e.centerY()};
            found = true;
        }
        return true;
    });
    return found;
}

bool AimAssist::locate(uint32_t id, Kind kind, Target& out)
{
    cWorldView* view = worldView();
    PickEntry e;
    if (!view || !id || !findEntry(view, id, e))
    {
        return false;
    }
    if (kind == Kind::Enemy && !isEnemy(heroCreature(), creatureOf(id)))
    {
        return false;
    }
    out = {id, e.centerX(), e.centerY()};
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
