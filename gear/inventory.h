#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The game's inventories as mods see them: the 32 containers of the inventory manager (a hero's backpack, the
// chest ...), the items in them, and items taken out, copied to bytes and made again. Everything reads the game's
// memory with a fault guard and calls its own inventory and object code, like the engine does when the player moves
// an item, so the cells, the items' owners and the object lists stay consistent. Engine thread only (the Lua host's
// thread). Changes are single player only (World::changesAllowed).
namespace Inventory
{
    constexpr uint32_t kContainers = 0x20;
    constexpr uint32_t kChest = 0x11;   // the chest window's container

    struct Container
    {
        uint32_t index = 0;
        uint32_t columns = 0;
        uint32_t rows = 0;
    };

    struct Entry
    {
        uint32_t id = 0;        // the item object
        uint32_t x = 0;         // top-left cell
        uint32_t y = 0;
        uint32_t width = 0;     // in cells
        uint32_t height = 0;
    };

    // False before a game ran, or if the addresses were not found.
    bool available();

    // Container `index` (0..31); false if it is not set up (no columns or rows).
    bool container(uint32_t index, Container& out);

    // The items in the container, in cell order.
    std::vector<Entry> items(uint32_t index);

    // The size in cells of item `id`; false if it is no item.
    bool itemSize(uint32_t id, uint32_t& width, uint32_t& height);

    // Room for a width x height item: its top-left cell.
    bool findSpace(uint32_t index, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y);

    // Puts item `id` (taken out of the world or of another container first) at cell (x, y); false with `error`.
    bool put(uint32_t index, uint32_t id, uint32_t x, uint32_t y, const char*& error);

    // Takes item `id` out of container `index`: the item object stays, owned by nobody.
    bool remove(uint32_t index, uint32_t id, const char*& error);

    // Destroys item `id`, which must be in no container.
    bool destroy(uint32_t id, const char*& error);

    // Puts upgrade item `upgradeId` (a rune lying in the world or in no container) into a free socket of item
    // `itemId`, as the blacksmith does: the item gains its bonuses and an entry, the upgrade item is consumed.
    bool socket(uint32_t itemId, uint32_t upgradeId, const char*& error);

    // Copies item `id` to bytes: its type and its own data (the 0x82-byte entries and record the engine's network
    // code sends), not where it is. Format: u32 type, u16 entry count, the entries, the record.
    bool exportItem(uint32_t id, std::string& bytes, const char*& error);

    // Makes an item from those bytes at the hero's feet (a new object of the same type with the saved data); its
    // id, or 0 with `error`. The caller puts it into a container.
    uint32_t importItem(const std::string& bytes, const char*& error);
}
