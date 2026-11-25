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

    // The container nobody uses: temporary display items (icons, tooltips) are put into it so they are never in the world.
    constexpr uint32_t kScratch = 31;

    // While one of these lives, the item and inventory calls of this module stay on this PC, in a network game too: an
    // item made is not announced (the engine takes its id from the pool the server granted this client, as for its own
    // local objects), put in and taken out without the announcing flag, and destroyed through the engine's local path
    // (no 0x167; the id goes back to the pool). For temporary items drawn or read and gone within the frame. Nestable.
    struct LocalOnly
    {
        LocalOnly();
        ~LocalOnly();
        LocalOnly(const LocalOnly&) = delete;
        LocalOnly& operator=(const LocalOnly&) = delete;
    };

    // The merchant window's stock grid (set by Shop before it edits it): local-only scopes may take items out of it.
    void setShopContainer(uint32_t index);

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
    // The type id inside exported bytes (0 if they are no export).
    uint32_t exportedType(const std::string& bytes);

    // Gives a constructed item object, registered or not (a scratch object made by the object factory), the data of an
    // export: its entries, its record (and the hash), and a weapon's extra dwords. The object must be of the export's type.
    bool fillObject(uintptr_t object, const std::string& bytes, const char*& error);

    // `temporary`: only to read something from it and destroy it at once (tooltips): made local-only in a network game.
    // In a network game the item is made and announced with the engine's own network calls (0x16F, then 0x16C with its
    // data) and put into the local hero's backpack or the chest with the engine's announcing calls.
    uint32_t importItem(const std::string& bytes, const char*& error, bool temporary = false);
}
