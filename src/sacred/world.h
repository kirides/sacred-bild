#pragma once
#include "game/sacred_addr.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>

// The world view: the isometric world drawn on the screen (cWorldView, its subclass cWorldView0 is the one in game).
namespace Sacred
{
    // A position in the world, as screenToWorld gives it.
    struct WorldPoint
    {
        uint16_t _00;
        uint8_t _02[2];
        int32_t x, y;
        uint8_t layer;
        uint8_t _0d[3];
    };
    static_assert(sizeof(WorldPoint) == 0x10 && offsetof(WorldPoint, x) == 0x04 && offsetof(WorldPoint, layer) == 0x0C);

    // An entry of the world view's pick list: an object's rect on the screen (physical pixels). Ids with bit 31 set
    // are not objects (the picker ranks them separately).
    struct PickEntry
    {
        uint32_t id;
        int32_t x, y;               // left, top
        int16_t width, height;
        uint8_t _10[0x1C - 0x10];

        float centerX() const { return x + width * 0.5f; }
        float centerY() const { return y + height * 0.5f; }
    };
    static_assert(sizeof(PickEntry) == 0x1C && offsetof(PickEntry, width) == 0x0C);

    struct cWorldView
    {
        struct Vtable
        {
            void* _00[6];
            // Physical screen pixels to the world position under them.
            void(__fastcall* screenToWorld)(cWorldView* self, void* edx, int x, int y, WorldPoint* out);
        };

        const Vtable* vtable;
        uint8_t _04[0x96B0C - 0x04];
        // What can be picked on the screen, rebuilt by the world renderer every frame: a std::vector guarded by
        // pickLock. The world pick (Addr::worldPick) gives up on more than 1000 entries.
        CRITICAL_SECTION pickLock;
        PickEntry* pickBegin;
        PickEntry* pickEnd;

        void screenToWorld(int x, int y, WorldPoint& out) { vtable->screenToWorld(this, nullptr, x, y, &out); }

        // The object id under (x, y) (physical pixels) ranked as the cursor does, 0 for none; the hit's rect goes to
        // rect (x, y, width | height << 16) if given.
        uint32_t pick(int x, int y, uint32_t excludeId, int32_t* rect) { return Addr::worldPick(this, x, y, excludeId, rect); }
    };
    static_assert(offsetof(cWorldView::Vtable, screenToWorld) == 0x18);
    static_assert(offsetof(cWorldView, pickLock) == 0x96B0C);
    static_assert(offsetof(cWorldView, pickBegin) == 0x96B24 && offsetof(cWorldView, pickEnd) == 0x96B28);
}
