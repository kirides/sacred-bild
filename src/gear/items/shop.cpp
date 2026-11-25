#include "shop.h"
#include "inventory.h"
#include "log.h"
#include "lua_host.h"
#include "world/world.h"
#include "engine/gear_addr.h"
#include "patch.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    using namespace Gear;

    // The merchant window (cUI_Merchandise): +0x154 the stock grid, +0x3E8 the merchant's object id, +0x3F0 sixteen {category dword, weight word,
    // word} entries. The window manager keeps it at +0x90 (the fifth window slot).
    constexpr uintptr_t kWindowSlot = 0x90, kGrid = 0x154, kMerchant = 0x3E8, kCategories = 0x3F0, kGridNumber = 0x1160, kVisible = 0x10;
    constexpr int kCategoryCount = 16;
    constexpr uintptr_t kHeroLevel = 0x3FE;

    decltype(Addr::merchantReceive)::Ptr g_origReceive = nullptr;
    decltype(Addr::merchantClear)::Ptr g_origClear = nullptr;
    decltype(Addr::merchantRestock)::Ptr g_origRestock = nullptr;
    decltype(Addr::merchantAddItem)::Ptr g_origAdd = nullptr;
    decltype(Addr::itemGenerate)::Ptr g_origGenerate = nullptr;
    std::atomic<bool> g_active{false};
    std::vector<uintptr_t> g_pages;                 // the merchant window's stock pages (window objects), learned when they stock
    uintptr_t g_pageVtable = 0;

    // What the engine did inside one run of the window's handler.
    struct Frame
    {
        int clearAll = 0, clearSome = 0, restock = 0, added = 0;
    };
    thread_local Frame* t_frame = nullptr;
    thread_local bool t_api = false;            // inside Shop::add: the engine's calls are the mod's
    thread_local int64_t t_level = -1;          // the generator's level while Shop::add runs, -1 for the engine's own

    template <class T>
    bool readAt(uintptr_t address, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t windowAddress()
    {
        uintptr_t manager = 0, window = 0;
        if (!Addr::g_pUiManager || !readAt(reinterpret_cast<uintptr_t>(Addr::g_pUiManager.get()), manager) || !manager ||
            !readAt(manager + kWindowSlot, window))
        {
            return 0;
        }
        return window;
    }

    bool gridOf(uintptr_t window, uintptr_t& grid, uint32_t& number);

    // Registers a window that has just stocked as a page (and forgets pages that no longer look like one).
    void learnPage(uintptr_t window)
    {
        uint32_t vtable = 0;
        if (!readAt(window, vtable))
        {
            return;
        }
        if (vtable != g_pageVtable)
        {
            g_pages.clear();
            g_pageVtable = vtable;
        }
        if (std::find(g_pages.begin(), g_pages.end(), window) == g_pages.end())
        {
            g_pages.push_back(window);
        }
    }

    // The learned pages that are still windows of the same class with a grid.
    std::vector<uintptr_t> livePages()
    {
        std::vector<uintptr_t> out;
        for (uintptr_t w : g_pages)
        {
            uint32_t vtable = 0;
            uintptr_t grid = 0;
            uint32_t number = 0;
            if (readAt(w, vtable) && vtable == g_pageVtable && gridOf(w, grid, number))
            {
                out.push_back(w);
            }
        }
        return out;
    }

    // The page window of container `container` (0: the first page by container number).
    uintptr_t pageWindow(uint32_t container)
    {
        uintptr_t best = 0;
        uint32_t bestNumber = 0xFFFFFFFF;
        for (uintptr_t w : livePages())
        {
            uintptr_t grid = 0;
            uint32_t number = 0;
            gridOf(w, grid, number);
            if ((container && number == container) || (!container && number < bestNumber))
            {
                best = w;
                bestNumber = number;
                if (container)
                {
                    break;
                }
            }
        }
        return best;
    }

    uint32_t heroLevel()
    {
        void* manager = nullptr;
        if (!Addr::g_pObjectManager || !Addr::cObjectManager_hero || !readAt(reinterpret_cast<uintptr_t>(Addr::g_pObjectManager.get()), manager) || !manager)
        {
            return 0;
        }
        const uintptr_t hero = reinterpret_cast<uintptr_t>(Addr::cObjectManager_hero(manager));
        uint16_t level = 0;
        return hero && readAt(hero + kHeroLevel, level) ? level : 0;
    }

    // The window's stock grid and its container number.
    bool gridOf(uintptr_t window, uintptr_t& grid, uint32_t& number)
    {
        uint16_t n = 0;
        if (!window || !readAt(window + kGrid, grid) || !grid || !readAt(grid + kGridNumber, n) || n >= Inventory::kContainers)
        {
            return false;
        }
        number = n;
        return true;
    }

    // The engine's one-item stocker, fault-guarded (no C++ objects here: __try).
    bool callStock(uintptr_t window, uint32_t type, uint32_t quality)
    {
        __try
        {
            Addr::merchantAddItem(reinterpret_cast<void*>(window), type, quality, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The item ids in a page's grid, sorted.
    std::vector<uint32_t> wareIds(uint32_t container)
    {
        std::vector<uint32_t> ids;
        for (const Inventory::Entry& e : Inventory::items(container))
        {
            ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    uint32_t __fastcall hookClear(void* self, void* edx, uint32_t all)
    {
        if (t_frame)
        {
            if (all & 0xFF)
            {
                ++t_frame->clearAll;
            }
            else
            {
                ++t_frame->clearSome;
            }
        }
        return g_origClear(self, edx, all);
    }

    void __fastcall hookRestock(void* self, void* edx, uint32_t count)
    {
        if (t_frame)
        {
            ++t_frame->restock;
        }
        g_origRestock(self, edx, count);
    }

    uint32_t __fastcall hookAdd(void* self, void* edx, uint32_t type, uint32_t quality, uint32_t zero)
    {
        if (t_frame && !t_api)
        {
            ++t_frame->added;
        }
        return g_origAdd(self, edx, type, quality, zero);
    }

    void __cdecl hookGenerate(uint32_t level, uint32_t quality, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t item, uint32_t type, uint32_t a9)
    {
        if (t_api && t_level >= 0)
        {
            level = static_cast<uint32_t>(t_level);
        }
        g_origGenerate(level, quality, a3, a4, a5, a6, item, type, a9);
    }

    uint32_t __fastcall hookReceive(void* self, void* edx, void* event)
    {
        if (t_frame)
        {
            return g_origReceive(self, edx, event);
        }
        const uintptr_t window = reinterpret_cast<uintptr_t>(self);
        uint32_t before = 0;
        readAt(window + kMerchant, before);
        uintptr_t grid = 0;
        uint32_t number = 0;
        const bool paged = gridOf(window, grid, number);
        std::vector<uint32_t> wares = paged ? wareIds(number) : std::vector<uint32_t>();
        Frame frame;
        t_frame = &frame;
        const uint32_t result = g_origReceive(self, edx, event);
        t_frame = nullptr;
        uint32_t after = 0;
        readAt(window + kMerchant, after);
        const char* reason = nullptr;
        if (frame.clearAll)
        {
            reason = after != before ? "new" : "refresh";
        }
        else if ((frame.clearSome || frame.restock || frame.added) && paged && wareIds(number) != wares)
        {
            // Only when the engine really changed the page: its churn of a reopened merchant often changes nothing.
            reason = "restock";
        }
        if (reason && gridOf(window, grid, number))
        {
            learnPage(window);
            if (g_active.load(std::memory_order_relaxed))
            {
                LuaHost::shopStock(reason, after, number, heroLevel());
            }
        }
        return result;
    }
}

bool Shop::install()
{
    if (!Addr::merchantReceive || !Addr::merchantClear || !Addr::merchantRestock || !Addr::merchantAddItem || !Addr::itemGenerate)
    {
        return false;
    }
    bool ok = Patch::hook(g_origReceive, Addr::merchantReceive, hookReceive, "merchantReceive");
    ok = Patch::hook(g_origClear, Addr::merchantClear, hookClear, "merchantClear") && ok;
    ok = Patch::hook(g_origRestock, Addr::merchantRestock, hookRestock, "merchantRestock") && ok;
    ok = Patch::hook(g_origAdd, Addr::merchantAddItem, hookAdd, "merchantAddItem") && ok;
    ok = Patch::hook(g_origGenerate, Addr::itemGenerate, hookGenerate, "itemGenerate") && ok;
    return ok;
}

void Shop::setActive(bool active)
{
    g_active.store(active);
}

bool Shop::info(Info& out)
{
    out = Info{};
    const uintptr_t parent = windowAddress();
    uint32_t parentFlags = 0;
    readAt(parent + kVisible, parentFlags);
    for (uintptr_t window : livePages())
    {
        uintptr_t grid = 0;
        uint32_t number = 0, merchant = 0, flags = 0;
        gridOf(window, grid, number);
        readAt(window + kMerchant, merchant);
        readAt(window + kVisible, flags);
        Page page;
        page.container = number;
        page.visible = (flags & 1) != 0;
        for (int i = 0; i < kCategoryCount; ++i)
        {
            uint32_t type = 0;
            uint16_t weight = 0;
            if (!readAt(window + kCategories + i * 8, type) || !readAt(window + kCategories + i * 8 + 4, weight))
            {
                break;
            }
            if (type)
            {
                page.categories.push_back({type, weight});
            }
        }
        if (!out.merchant)
        {
            out.merchant = merchant;
        }
        out.open = out.open || page.visible;
        out.pages.push_back(std::move(page));
    }
    if (out.pages.empty() || !out.merchant)
    {
        return false;
    }
    out.open = out.open || (parentFlags & 1) != 0;
    std::sort(out.pages.begin(), out.pages.end(), [](const Page& a, const Page& b) { return a.container < b.container; });
    out.heroLevel = heroLevel();
    return true;
}

uint32_t Shop::pick(uint32_t category, uint32_t level, uint32_t quality, uint32_t heroPref)
{
    void* types = nullptr;
    if (!Addr::cItem_pickType || !Addr::g_pTypeManager || !readAt(reinterpret_cast<uintptr_t>(Addr::g_pTypeManager.get()), types) || !types)
    {
        return 0;
    }
    __try
    {
        // The shop's own call (0x6DA9C0): the third argument is twice the hero's level plus 100, the fourth 0xF, then the hero preference, flags 0, 1.
        return Addr::cItem_pickType(types, category, level & 0xFFFF, heroLevel() * 2 + 100, 0xF, heroPref, 0, 1, quality);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool Shop::add(uint32_t container, uint32_t type, uint32_t quality, uint32_t level, uint32_t& id, const char*& error)
{
    const uintptr_t window = pageWindow(container);
    uintptr_t grid = 0;
    uint32_t number = 0;
    if (!Addr::merchantAddItem || !window || !gridOf(window, grid, number))
    {
        error = "no such shop page (no merchant has stocked the window yet)";
        return false;
    }
    if (type == 0 || type >= 0x2000)
    {
        error = "bad item type";
        return false;
    }
    const std::vector<Inventory::Entry> before = Inventory::items(number);
    t_api = true;
    t_level = level ? static_cast<int64_t>(level) : -1;
    const bool faulted = !callStock(window, type, quality);
    t_api = false;
    t_level = -1;
    if (faulted)
    {
        LOG("Shop: stocking type {} faulted", type);
        error = "the game faulted while stocking the item";
        return false;
    }
    id = 0;
    for (const Inventory::Entry& e : Inventory::items(number))
    {
        bool known = false;
        for (const Inventory::Entry& b : before)
        {
            known = known || b.id == e.id;
        }
        if (!known)
        {
            id = e.id;
            break;
        }
    }
    if (!id)
    {
        error = "no room in the shop's grid, or the game refused the item";
        return false;
    }
    return true;
}

bool Shop::remove(uint32_t container, uint32_t id, const char*& error)
{
    const uintptr_t window = pageWindow(container);
    uintptr_t grid = 0;
    uint32_t number = 0;
    if (!window || !gridOf(window, grid, number))
    {
        error = "no such shop page";
        return false;
    }
    Inventory::LocalOnly local;
    Inventory::setShopContainer(number);
    return Inventory::remove(number, id, error) && Inventory::destroy(id, error);
}
