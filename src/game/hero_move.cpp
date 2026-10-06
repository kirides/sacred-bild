#include "game/hero_move.h"
#include "game/frame_hooks.h"
#include "game/sacred_addr.h"
#include "input/inject.h"

#include <windows.h>
#include <cstdint>

namespace
{
    using namespace Sacred;

    // thiscall targets are called as fastcall with an unused EDX parameter.
    using GetDataFn = void*(__fastcall*)(void* manager, void* edx, uint32_t id);
    using DynamicCastFn = void*(__cdecl*)(void* object, long vfDelta, void* source, void* target, int isReference);
    using ScreenToWorldFn = void(__fastcall*)(void* view, void* edx, int x, int y, void* out);
    using SendOrderFn = void(__fastcall*)(void* engine, void* edx, void* creature, void* order);
    using MouseInstanceFn = void*(__cdecl*)();

    // The call's context: x | y << 15, kKeep for keepWalking, kStop or kRelease.
    constexpr uintptr_t kRelease = 1u << 29;
    constexpr uintptr_t kKeep = 1u << 30;
    constexpr uintptr_t kStop = 1u << 31;

    // The window thread's: where the walking creature stood at the last order or check.
    void* g_walker = nullptr;
    int32_t g_lastX = 0, g_lastY = 0;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    // The creature the mouse commands, as the world mouse handler finds it; nullptr while the world takes no mouse.
    void* controlled(void* engine)
    {
        void* manager = *reinterpret_cast<void**>(Addr::g_pObjectManager);
        if (!manager || (member<uint32_t>(engine, Engine::flags) & Engine::noWorldInput) ||
            (*reinterpret_cast<const uint8_t*>(Addr::g_worldInputFlags) & 2))
        {
            return nullptr;
        }
        const uint32_t id = member<uint32_t>(engine, Engine::controlled);
        void* object = (id & 0x80000000u) ? nullptr :
            reinterpret_cast<GetDataFn>(Addr::cObjectManager_getData)(manager, nullptr, id);
        if (!object)
        {
            return nullptr;
        }
        return reinterpret_cast<DynamicCastFn>(Addr::rtDynamicCast)(object, 0,
            reinterpret_cast<void*>(Addr::cObject_typeDescriptor), reinterpret_cast<void*>(Addr::cCreature_typeDescriptor), 0);
    }

    // The cMouse's left-button-held bit, as the window procedure keeps it.
    void holdLeft(bool held)
    {
        if (void* mouse = reinterpret_cast<MouseInstanceFn>(Addr::cMouse_instance)())
        {
            auto& flags = member<uint16_t>(mouse, Mouse::flags);
            flags = held ? static_cast<uint16_t>(flags | Mouse::leftHeld) : static_cast<uint16_t>(flags & ~Mouse::leftHeld);
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
        void* engine = FrameHooks::engine();
        void* creature = engine ? controlled(engine) : nullptr;
        if (!creature)
        {
            g_walker = nullptr;
            return;
        }
        const int32_t x = member<int32_t>(creature, Object::x), y = member<int32_t>(creature, Object::y);
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
        alignas(4) uint8_t order[Order::size] = {};
        member<uintptr_t>(order, 0) = Addr::cOrder_vtable;
        member<uint32_t>(order, Order::type) = 4;
        if (!(packed & kStop))
        {
            const uint16_t index = member<uint16_t>(engine, Engine::viewIndex);
            void* view = index < 8 ? member<void*>(engine, Engine::views + index * 4) : nullptr;
            if (!view)
            {
                return;
            }
            alignas(4) uint8_t point[WorldPoint::size] = {};
            const auto toWorld = reinterpret_cast<ScreenToWorldFn>((*static_cast<uintptr_t**>(view))[6]);
            toWorld(view, nullptr, static_cast<int>(packed & 0x7FFF), static_cast<int>((packed >> 15) & 0x7FFF), point);
            member<uint32_t>(order, Order::mode) = 2;
            member<int32_t>(order, Order::x) = member<int32_t>(point, WorldPoint::x);
            member<int32_t>(order, Order::y) = member<int32_t>(point, WorldPoint::y);
            member<uint32_t>(order, Order::follow) = 1;
        }
        reinterpret_cast<SendOrderFn>(Addr::cEngine_sendOrder)(engine, nullptr, creature, order);
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
