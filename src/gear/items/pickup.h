#pragma once
#include <cstdint>
#include <vector>

// Pickup rules: mods name items the local hero must not pick up and items whose ground labels are dimmed.
//
// The game moves a world item to a creature in one function (cCreature::pickupItem, ENG 00558080); Gear returns
// "not picked up" there for the local hero and a blocked item, before anything changes. The collect behaviour's
// filter (ENG 004EC330) gets the same answer, so the hero does not keep walking to a blocked item. Labels: the
// item label colour (ENG 005C13C0) gets its alpha scaled for dimmed items.
//
// Single player (and a host's own hero): a network client asks the host with a message of its own (0x141) and does
// not run pickupItem, so the rules do not apply there.
namespace Pickup
{
    // Inside the Patch transaction.
    bool install();

    // Lua thread: replaces the rules. `dimAlpha` 0..255 (255: labels as they are).
    void setRules(std::vector<uint32_t> blocked, std::vector<uint32_t> dimmed, uint8_t dimAlpha);

    // The colour the game gives item `id`'s ground label (ARGB: set green, the rarity's colour, a class's colour for
    // class items unless UNIQUE_COLOR is on), without a mod's dimming. Engine thread; false if there is no such item.
    bool labelColor(uint32_t id, uint32_t& color);

    // Pickups refused since the start (for mods and the log).
    uint32_t refused();
}
