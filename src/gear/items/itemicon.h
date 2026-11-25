#pragma once
// Item icons for mod UI: draws what the game's inventory windows show for an item, its own Granny model, through the
// game's model renderer (cInventoryEntry_render, 0x5DC430, does the same per item). Runs on the render thread inside
// the UI pass (src/gear/ui/ui.cpp).
#include <cstdint>

namespace ItemIcon
{
    // What to draw: the live item object `itemId` (its own model state and flags), or, without one, an item of
    // `type`: a temporary item object made at the hero's feet for the frame and destroyed after the layer's pass (only
    // in a single player game). The model is drawn centered in the box (x, y, w, h) of the 1024x768 layout, as large
    // as fits, keeping its aspect (the inventory's size is 32 units a cell).
    struct Request
    {
        uint32_t itemId = 0;
        uint32_t type = 0;
        float x = 0, y = 0, w = 0, h = 0;
    };

    // Draws one icon with the game's states (the caller gave them back); false if it cannot (no such item, addresses
    // missing, the type has no model). The device's states, transforms, viewport and the game's render flags are as
    // they were afterwards.
    bool draw(void* device, const Request& request);

    // After a layer's commands: takes the temporary items back out of their container and destroys them.
    void endPass();

    // Whether the addresses it needs were found.
    bool available();
}
