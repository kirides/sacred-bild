#include "master.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    // The tab's layout (the decompiled functions 006D5B80, 006D4610, 006D5760): four rune slots, 0x114 bytes apart from +0x154, each with
    // the item it holds at +0xFC (tab +0x250); the control's size word at +0x2C (width, height << 16); the chosen rune (item type) at tab +0x7B4, the one
    // shown at +0x79C; the visible bit in the control flags at +0x10.
    constexpr uintptr_t kFlags = 0x10, kSlots = 0x154, kSlotStride = 0x114, kSlotItem = 0xFC, kSlotSize = 0x2C, kShown = 0x79C, kChosen = 0x7B4;
    constexpr int kSlotCount = 4, kPrices = 5;

    std::atomic<bool> g_priceActive{false}, g_layoutActive{false}, g_exchangeActive{false};

    void(__fastcall* g_origRender)(void* tab, void* edx, void* device) = nullptr;
    void(__fastcall* g_origLayout)(void* tab) = nullptr;
    void(__fastcall* g_origExchange)(void* tab) = nullptr;

    // The engine's own price table entries, kept when the first answer of a mod replaces one.
    uint32_t g_vanilla[kPrices] = {};
    bool g_replaced[kPrices] = {};

    template <class T>
    bool read(uintptr_t base, uintptr_t offset, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int runesInSlots(uintptr_t tab)
    {
        int count = 0;
        for (int i = 0; i < kSlotCount; ++i)
        {
            uint32_t item = 0;
            if (read(tab, kSlots + i * kSlotStride + kSlotItem, item) && item != 0)
            {
                ++count;
            }
        }
        return count;
    }

    // The ability an item type teaches (the type record's ability word): 0 when it is no rune.
    int abilityOf(int rune)
    {
        if (!Gear::Addr::typeAbilityOfRune || !Gear::Addr::g_pTypeManager || rune <= 0)
        {
            return 0;
        }
        __try
        {
            void* manager = *reinterpret_cast<void**>(Gear::Addr::g_pTypeManager.address);
            return manager ? static_cast<int>(Gear::Addr::typeAbilityOfRune(manager, rune) & 0xFFFF) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Puts what the mods answer for the current state into the engine's table entry for the number of runes (the engine's own value when no
    // mod answers), so every place that reads the table sees the same price.
    void refreshPrice(uintptr_t tab)
    {
        if (!Gear::Addr::masterPriceTable)
        {
            return;
        }
        const int count = runesInSlots(tab);
        if (count < 0 || count >= kPrices)
        {
            return;
        }
        uint32_t* entry = Gear::Addr::masterPriceTable.get() + count;
        double price = 0;
        bool answered = false;
        if (g_priceActive.load(std::memory_order_relaxed))
        {
            uint32_t shown = 0, chosen = 0;
            read(tab, kShown, shown);
            read(tab, kChosen, chosen);
            const int rune = static_cast<int>(shown ? shown : chosen);
            answered = LuaHost::masterEvent("master.price", rune, abilityOf(rune), count, price) == LuaHost::Answer::Value && price >= 0 &&
                       price < 2147483647.0;
        }
        if (answered)
        {
            if (!g_replaced[count])
            {
                g_vanilla[count] = *entry;
                g_replaced[count] = true;
            }
            *entry = static_cast<uint32_t>(price);
        }
        else if (g_replaced[count])
        {
            *entry = g_vanilla[count];
            g_replaced[count] = false;
        }
    }

    // The engine thread draws the tab every frame while the window is open: the price is refreshed before it draws and before the next click
    // reads it. (The tab's event handler reads the entry the last frame wrote.)
    void __fastcall hookRender(void* tab, void* edx, void* device)
    {
        uint32_t flags = 0;
        if (g_priceActive.load(std::memory_order_relaxed) && read(reinterpret_cast<uintptr_t>(tab), kFlags, flags) && (flags & 1))
        {
            refreshPrice(reinterpret_cast<uintptr_t>(tab));
        }
        g_origRender(tab, edx, device);
    }

    void __fastcall hookLayout(void* tab)
    {
        g_origLayout(tab);
        double hide = 0;
        if (g_layoutActive.load(std::memory_order_relaxed) &&
            LuaHost::masterEvent("master.layout", 0, 0, 0, hide) == LuaHost::Answer::Value && hide != 0)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(tab);
            for (int i = 0; i < kSlotCount; ++i)
            {
                const uint32_t none = 0;
                __try
                {
                    std::memcpy(reinterpret_cast<void*>(base + kSlots + i * kSlotStride + kSlotSize), &none, sizeof(none));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }
    }

    void __fastcall hookExchange(void* tab)
    {
        if (g_exchangeActive.load(std::memory_order_relaxed))
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(tab);
            uint32_t chosen = 0;
            read(base, kChosen, chosen);
            double handled = 0;
            if (LuaHost::masterEvent("master.exchange", static_cast<int>(chosen), abilityOf(static_cast<int>(chosen)), runesInSlots(base), handled) ==
                    LuaHost::Answer::Value &&
                handled != 0)
            {
                return;
            }
        }
        g_origExchange(tab);
    }
}

bool Master::install()
{
    using namespace Gear;
    if (!Addr::masterRenderFn || !Addr::masterLayoutFn || !Addr::masterExchangeFn || !Addr::masterPriceTable)
    {
        LOG("Master: the combo master's rune exchange functions were not found: master.* events are off");
        return false;
    }
    bool ok = Patch::hook(g_origRender, Addr::masterRenderFn, hookRender, "masterRenderFn");
    ok = Patch::hook(g_origLayout, Addr::masterLayoutFn, hookLayout, "masterLayoutFn") && ok;
    ok = Patch::hook(g_origExchange, Addr::masterExchangeFn, hookExchange, "masterExchangeFn") && ok;
    return ok;
}

void Master::setActive(const char* event, bool active)
{
    if (std::strcmp(event, "master.price") == 0)
    {
        g_priceActive.store(active);
    }
    else if (std::strcmp(event, "master.layout") == 0)
    {
        g_layoutActive.store(active);
    }
    else if (std::strcmp(event, "master.exchange") == 0)
    {
        g_exchangeActive.store(active);
    }
}
