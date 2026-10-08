#pragma once
#include "game/sacred_addr.h"

#include <cstddef>
#include <cstdint>

// The mouse (cMouse, one instance): the cursor the UI and the world poll, and the cursor image drawn at it.
namespace Sacred
{
    struct cMouse
    {
        // setPosition (ENG 006554E0, called on WM_MOUSEMOVE) copies x/y to the drawn position first, then sets them;
        // with bit 0 of the flags it clamps them to `clamp` instead when the previous position was outside it.
        uint16_t flags;
        uint8_t _02[2];
        int32_t x, y;
        int32_t drawnX, drawnY;
        int32_t clamp[4];           // left, top, right, bottom
        uint8_t _24[0x64 - 0x24];
        void* cursorImage;          // getCursorPos writes its outputs only while this is set
        void* heldItem;             // the item the cursor carries

        // In flags: the left button is held. Only the window procedure sets and clears it (button messages); a
        // player's follow-the-cursor walk (move executor ENG 004F4770, at 004F681A) ends as soon as it is clear.
        static constexpr uint16_t leftHeld = 0x100;

        // Made on first use.
        static cMouse* instance() { return Addr::cMouse_instance(); }

        // The drawn cursor's top-left corner (drawn position minus the cursor's hot spot); writes nothing while no
        // cursor image is set.
        void cursorPos(int& left, int& top) { Addr::cMouse_getCursorPos(this, &left, &top); }
    };
    static_assert(offsetof(cMouse, x) == 0x04 && offsetof(cMouse, drawnX) == 0x0C && offsetof(cMouse, clamp) == 0x14);
    static_assert(offsetof(cMouse, cursorImage) == 0x64 && offsetof(cMouse, heldItem) == 0x68);
}
