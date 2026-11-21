#include "game/hero_move.h"
#include "game/frame_hooks.h"
#include "input/inject.h"
#include "sacred/engine.h"
#include "sacred/mouse.h"

#include <windows.h>
#include <cstdint>

namespace
{
    using namespace Sacred;

    // The call's context: x | y << 15, kKeep for keepWalking, kStop or kRelease.
    constexpr uintptr_t kRelease = 1u << 29;
    constexpr uintptr_t kKeep = 1u << 30;
    constexpr uintptr_t kStop = 1u << 31;

    // The window thread's: where the walking creature stood at the last order or check.
    cCreature* g_walker = nullptr;
    int32_t g_lastX = 0, g_lastY = 0;

    // The creature the mouse commands, as the world mouse handler finds it; nullptr while the world takes no mouse.
    cCreature* controlled(cEngine* engine)
    {
        cObjectManager* manager = cObjectManager::instance();
        if (!manager || (engine->flags & cEngine::noWorldInput) || (*Addr::g_worldInputFlags & 2))
        {
            return nullptr;
        }
        return manager->creature(engine->controlled);
    }

    // The cMouse's left-button-held bit, as the window procedure keeps it.
    void holdLeft(bool held)
    {
        if (cMouse* mouse = cMouse::instance())
        {
            mouse->flags = held ? static_cast<uint16_t>(mouse->flags | cMouse::leftHeld)
                                : static_cast<uint16_t>(mouse->flags & ~cMouse::leftHeld);
        }
    }

    // On the window's thread.
    void send(void* context)
    {
        const auto packed = reinterpret_cast<uintptr_t>(context);
        if (packed & (kStop | kRelease))
        {
            holdLeft(false);
            if (packed & kRelease)
            {
                return;
            }
        }
        cEngine* engine = FrameHooks::engine();
        cCreature* creature = engine ? controlled(engine) : nullptr;
        if (!creature)
        {
            g_walker = nullptr;
            return;
        }
        const int32_t x = creature->x, y = creature->y;
        if (packed & kKeep)
        {
            const bool moved = creature != g_walker || x != g_lastX || y != g_lastY;
            g_walker = creature;
            g_lastX = x;
            g_lastY = y;
            if (moved)
            {
                return;
            }
        }
        g_walker = creature;
        g_lastX = x;
        g_lastY = y;
        if (!(packed & kStop))
        {
            holdLeft(true);
        }
        cOrder order;
        order.type = cOrder::move;
        if (!(packed & kStop))
        {
            cWorldView* view = engine->worldView();
            if (!view)
            {
                return;
            }
            WorldPoint point{};
            view->screenToWorld(static_cast<int>(packed & 0x7FFF), static_cast<int>((packed >> 15) & 0x7FFF), point);
            order.mode = cOrder::walk;
            order.x = point.x;
            order.y = point.y;
            order.follow = 1;
        }
        engine->sendOrder(creature, order);
    }
}

void HeroMove::follow(int x, int y)
{
    const uintptr_t packed = static_cast<uintptr_t>(x & 0x7FFF) | (static_cast<uintptr_t>(y & 0x7FFF) << 15);
    Inject::call(&send, reinterpret_cast<void*>(packed));
}

void HeroMove::keepWalking(int x, int y)
{
    const uintptr_t packed = static_cast<uintptr_t>(x & 0x7FFF) | (static_cast<uintptr_t>(y & 0x7FFF) << 15);
    Inject::call(&send, reinterpret_cast<void*>(packed | kKeep));
}

void HeroMove::stop()
{
    Inject::call(&send, reinterpret_cast<void*>(kStop));
}

void HeroMove::release()
{
    Inject::call(&send, reinterpret_cast<void*>(kRelease));
}
