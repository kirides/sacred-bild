#pragma once
#include "game/sacred_addr.h"
#include "sacred/object.h"
#include "sacred/world.h"

#include <cstddef>
#include <cstdint>

// The engine (cEngine, one instance): the in-game loop, its views and the orders the player gives.
namespace Sacred
{
    // An order for a creature (cEngine::sendOrder). The world mouse handler (ENG 006172C0) builds its move orders on
    // the stack. Walk: target set, `follow` 1 makes the local hero walk straight at the cursor (ENG 004FB7E0
    // recomputes the target from the mouse every step, no path search, no cell snapping) as hold-to-walk does after
    // half a second; 0 path-finds to the snapped target. Stop: all zero but `type`.
    struct cOrder
    {
        uintptr_t vtable = Addr::cOrder_vtable;
        uint32_t type = 0;
        uint8_t _08[0x14 - 0x08] = {};
        uint32_t mode = 0;
        int32_t x = 0, y = 0;       // world target
        uint32_t follow = 0;
        uint8_t _24[0x44 - 0x24] = {};

        static constexpr uint32_t move = 4;     // type
        static constexpr uint32_t walk = 2;     // mode; 0 = stop
    };
    static_assert(sizeof(cOrder) == 0x44);
    static_assert(offsetof(cOrder, type) == 0x04 && offsetof(cOrder, mode) == 0x14);
    static_assert(offsetof(cOrder, x) == 0x18 && offsetof(cOrder, y) == 0x1C && offsetof(cOrder, follow) == 0x20);

    struct cEngine
    {
        uint8_t _00[0x08];
        cWorldView* views[8];       // the world view is views[viewIndex] (the world pick's this)
        uint8_t _28[0x48 - 0x28];
        uint16_t viewIndex;
        uint8_t _4a[0x54 - 0x4A];
        uint32_t flags;
        uint8_t _58[0x13C - 0x58];
        uint32_t controlled;        // the id of the creature the mouse commands (the hero, or its mount)

        // In flags.
        static constexpr uint32_t noWorldInput = 0x4000;    // the world mouse handler ignores the mouse
        static constexpr uint32_t loading = 0x10000;        // loading screen
        static constexpr uint32_t fadeOut = 0x20000;
        static constexpr uint32_t fadeIn = 0x40000;
        static constexpr uint32_t black = 0x80000;

        // The engine singleton.
        static cEngine* instance() { return Addr::cEngine_instance(-1); }

        // The view the cursor picks in, nullptr if there is none.
        cWorldView* worldView() const { return viewIndex < 8 ? views[viewIndex] : nullptr; }

        // How far the view trails the hero: the hero stands at the screen center minus twice that (the hold-to-walk
        // direction at ENG 004FB8AF is measured from there).
        void viewOffset(int& x, int& y) { Addr::cEngine_getViewOffset(this, &x, &y); }

        // Hands `order` to `creature` as the world mouse handler (ENG 006172C0) and its hold-to-walk (0060F130) do:
        // a walk target is snapped to a walkable cell first. The order may live on the stack, as theirs do.
        void sendOrder(cCreature* creature, cOrder& order) { Addr::cEngine_sendOrder(this, creature, &order); }
    };
    static_assert(offsetof(cEngine, views) == 0x08 && offsetof(cEngine, viewIndex) == 0x48);
    static_assert(offsetof(cEngine, flags) == 0x54 && offsetof(cEngine, controlled) == 0x13C);
}
