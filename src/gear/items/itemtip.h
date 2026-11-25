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

    // The tooltip's text (with the game's colour codes) instead of a popup: of an item, or of exported bytes.
    bool text(uint32_t id, std::wstring& out, const char*& error);
    bool textExported(const std::string& bytes, std::wstring& out, const char*& error);
    // The tooltip's text of a new scratch item of item type `type` (no data filled in: the name and the plain lines), for the type's localized name.
    bool textForType(uint32_t type, std::wstring& out, const char*& error);

    // The item whose tooltip the game shows now (the reserved popup: the inventory, chest and shop windows, the equipment window,
    // the pad's cursor ... anything that rests the cursor on an item), as of the last frame: its id and the popup's rectangle in the
    // 1024x768 layout (the size is from the last layout: 0 before the first). False when no item tooltip is up.
    struct Hover
    {
        uint32_t id = 0;
        int x = 0, y = 0, width = 0, height = 0;
        float frameX = 0, frameY = 0;   // SacredBild's frame of the popup (the window that set its text): layout x = x + frameX
        bool framed = false;            // else x, y are canvas coordinates: layout x = x + the screen's left edge (ui.screen)
        uint32_t flags = 0;         // the popup's own flags (+0x154), diagnostics
    };
    bool hovered(Hover& out);

    // The equipment slot (0..18) the item type goes to for hero `creatureId`, or -1: asked of the engine with a copy of the hero
    // whose slots are empty, so it is the first slot of the item's group (rings 8..11, amulets 6, 7, hands 12, 13) whatever it wears.
    int equipSlot(uint32_t creatureId, uint32_t itemType);

    // Hook the popup's owner setter (before Patch::commit); count a frame (once per tick, before the mods run).
    bool install();
    void frame();

    // The engine's own line of bonus `slot` (0..7) of an item's value record (entry 0: the item's own, 1..n: the merged records of its
    // sockets and random properties, in the order gear.inv.export lists them): UTF-16, with the game's colour codes; empty when the
    // slot has no bonus. False when the item or entry does not exist.
    bool bonusLine(uint32_t id, uint32_t entry, uint32_t slot, std::wstring& out);

    // The same for an exported item.
    bool showExported(const std::string& bytes, int x, int y, int& width, int& height, const char*& error);
}
