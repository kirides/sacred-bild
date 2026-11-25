#pragma once
#include <cstdint>
#include <vector>

// The merchants' stock (docs/GearApi.md: `gear.shop`, event `shop.stock`).
//
// ENG 0x6DAD90 is the event handler of the merchant window (UI_WND_MERCHANT, window manager +0x90). The window keeps ONE stock grid (container
// at window +0x154) for the merchant it showed last (window +0x3E8, the merchant's object id). Opening a merchant that is not that one empties
// the grid (0x6D9B90 with all = 1), stocks the merchant's fixed wares and a random stock (0x6DA320) plus a few potions; opening the same merchant
// again only churns the stock: half of the time a random handful goes (0x6D9B90 with 0) and some random wares (0x6DA320) and potions come. The
// window's refresh message (kind 9) rebuilds it like a new merchant. Nothing is rolled when the player merely reopens the window, so the stock
// stays until the engine itself resets it.
//
// The hooks wrap the handler, count what the engine did inside it and, after it returns, run the mods' `shop.stock` event with the reason
// ('new', 'restock', 'refresh'); mods add and remove wares then. Everything a mod adds goes through the engine's one-item stocker 0x6D9DE0 (room in
// the grid, the item object, its properties from the item generator 0x5D5080), with the generator's level replaced when the mod asks for one.
namespace Shop
{
    bool install();

    // Whether some mod handles `shop.stock` (the pages are learned either way; the event only runs while one does).
    void setActive(bool active);

    struct Category
    {
        uint32_t type = 0;      // the ware category the stock generator draws (an equipment category, 5 and 8 .. 29; 9 potions)
        uint32_t weight = 0;
    };

    // One stock page of the merchant window. A merchant's wares are spread over three pages (the window's tabs), each its own grid with its own
    // categories and stocked on its own.
    struct Page
    {
        uint32_t container = 0;     // the page's grid: its container number (Inventory)
        bool visible = false;
        std::vector<Category> categories;
    };

    struct Info
    {
        bool open = false;          // a stock page is showing
        uint32_t merchant = 0;      // the merchant's object id (of the first page)
        uint32_t heroLevel = 0;
        std::vector<Page> pages;    // by container number
    };

    // False before a merchant stocked any page (no merchant, no grid).
    bool info(Info& out);

    // The item type the engine's picker gives for an equipment category at a level and quality (the call the shop's own generator makes); 0 if none.
    uint32_t pick(uint32_t category, uint32_t level, uint32_t quality, uint32_t heroPref);

    // Stocks one item of `type` through the engine; quality 0..14 (the generator's quality tier), level 0 for the engine's own (the hero's, spread).
    // `id` the new item's object id.
    // `container`: the page to stock (a number from `info`); 0 for the first page.
    bool add(uint32_t container, uint32_t type, uint32_t quality, uint32_t level, uint32_t& id, const char*& error);

    // Takes an item out of a page's grid and destroys it.
    bool remove(uint32_t container, uint32_t id, const char*& error);
}
