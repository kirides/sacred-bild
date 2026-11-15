#pragma once
#include <cstdint>
#include <string>

// The game's own item tooltip for mods: the framed popup the inventory windows show when the cursor rests on an item
// (name in the rarity colour, price, protection, level, bonus lines ...), for an item that exists, or for an item
// exported with Inventory::exportItem (a temporary item is made to read the text from and destroyed at once).
// Engine thread, single player for the temporary item.
namespace ItemTip
{
    // Shows item `id`'s tooltip centered on (x, y) of the 1024x768 layout (the popup keeps itself inside the screen).
    // `width` / `height`: the popup's size as of its last layout (the previous tooltip it showed), 0 before the first.
    bool show(uint32_t id, int x, int y, int& width, int& height, const char*& error);

    // The same for an exported item.
    bool showExported(const std::string& bytes, int x, int y, int& width, int& height, const char*& error);
}
