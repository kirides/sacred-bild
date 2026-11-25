#include "master.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "engine/gear_addr.h"

#include <windows.h>
#include <d3d.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    // The tab's layout (the decompiled functions 006D5B80, 006D4610, 006D5760): four rune slots, 0x114 bytes apart from +0x154, each with
    // the item it holds at +0xFC (tab +0x250); the control's size word at +0x2C (width, height << 16); the chosen rune (item type) at tab +0x7B4, the one
    // shown at +0x79C; the visible bit in the control flags at +0x10.
    constexpr uintptr_t kFlags = 0x10, kSlots = 0x154, kSlotStride = 0x114, kSlotItem = 0xFC, kSlotSize = 0x2C, kShown = 0x79C, kChosen = 0x7B4;
    constexpr int kSlotCount = 4, kPrices = 5;
    // The result box (a control at tab +0x69C: its position and size words at +0x24 / +0x28 / +0x2C), whose rect the "maximum level" message centers on;
    // the page's texts use the font 3 (the header the font 1).
    constexpr uintptr_t kResultControl = 0x69C;
    constexpr uint32_t kTextFont = 3;
    // The art description's place, in the pixels of the header's own rect (0x10, 0x3E): ReBorn's 01AC8087 draws it at 0x10, 0x8C.
    constexpr int32_t kDescribeX = 0x10, kDescribeY = 0x8C;
    // The "Maximum Level Attained" text (id 0x9327CD45: bit 31 sends the engine's lookup to its second table).
    constexpr uint32_t kMaximumText = 0x9327CD45, kWhite = 0xFFFFFFFF, kRed = 0xFFFF0000;

    std::atomic<bool> g_priceActive{false}, g_layoutActive{false}, g_exchangeActive{false}, g_describeActive{false}, g_maximumActive{false};

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

    struct cRect
    {
        int32_t x, y;
        int16_t w, h;
    };
    using DrawTextFn = int(__fastcall*)(void* font, void* edx, IDirect3DDevice7* device, cRect* rect, const wchar_t* text, uint32_t color, uint32_t flags);
    using TextWidthFn = uint16_t(__fastcall*)(void* font, void* edx, const wchar_t* text);

    // The font `id` of the game's font manager (a vector of cFont*), nullptr if there is none.
    void* fontById(uint32_t id)
    {
        const uintptr_t manager = Gear::Addr::g_pFontManager ? reinterpret_cast<uintptr_t>(*Gear::Addr::g_pFontManager) : 0;
        uintptr_t begin = 0, end = 0;
        if (!manager || !read(manager, 0, begin) || !read(manager, 4, end) || !begin || end < begin || id >= (end - begin) / 4)
        {
            return nullptr;
        }
        void* font = nullptr;
        return read(begin, id * 4, font) ? font : nullptr;
    }

    template <class Fn>
    Fn fontSlot(void* font, uintptr_t offset)
    {
        uintptr_t vtable = 0, slot = 0;
        return read(reinterpret_cast<uintptr_t>(font), 0, vtable) && vtable && read(vtable, offset, slot) ? reinterpret_cast<Fn>(slot) : nullptr;
    }

    // Draws `text` with the engine's own font draw (cFont vtable +0x18, as the page's header does) at x, y; a rect of no size lets the font lay the
    // text out on its own.
    void drawText(void* device, void* font, int32_t x, int32_t y, const wchar_t* text, uint32_t color)
    {
        auto draw = fontSlot<DrawTextFn>(font, 0x18);
        if (draw && device && text && *text)
        {
            cRect rect{x, y, 0, 0};
            __try
            {
                draw(font, nullptr, static_cast<IDirect3DDevice7*>(device), &rect, text, color, 0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }

    // The engine's own description of an art or spell at `level`, "<name> - Level <level>" (the art record's function 004D2020 or the spell record's
    // 004D27B0, both with the format 1). The engine builds it in a STLport string on its allocator.
    bool describeAbility(int ability, int level, std::wstring& out)
    {
        if (!Gear::Addr::abilityLookup1 || !Gear::Addr::abilityLookup2 || !Gear::Addr::abilityDescribe1 || !Gear::Addr::abilityDescribe2 ||
            !Gear::Addr::engine_alloc || !Gear::Addr::engine_free)
        {
            return false;
        }
        uint32_t text[3] = {};
        auto* buffer = static_cast<uint16_t*>(Gear::Addr::engine_alloc(0x10));
        if (!buffer)
        {
            return false;
        }
        buffer[0] = 0;
        text[0] = text[1] = reinterpret_cast<uint32_t>(buffer);
        text[2] = text[0] + 0x10;
        bool ok = false;
        __try
        {
            if (uint8_t* art = Gear::Addr::abilityLookup1(static_cast<int16_t>(ability)))
            {
                Gear::Addr::abilityDescribe1(art, text, static_cast<uint32_t>(level), 1);
                ok = true;
            }
            else if (uint8_t* spell = Gear::Addr::abilityLookup2(static_cast<int16_t>(ability)))
            {
                Gear::Addr::abilityDescribe2(spell, text, static_cast<uint32_t>(level), 1);
                ok = true;
            }
            if (ok && text[0] && text[1] >= text[0])
            {
                out.assign(reinterpret_cast<const wchar_t*>(text[0]), (text[1] - text[0]) / 2);
            }
            else
            {
                ok = false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        if (text[0])
        {
            Gear::Addr::engine_free(nullptr, reinterpret_cast<void*>(text[0]), (text[2] - text[0]) >> 1);
        }
        return ok;
    }

    // The red "maximum level" message ReBorn centers on the result box (its position word plus half of the difference of the widths), under the box.
    void drawMaximum(uintptr_t tab, void* device, void* font)
    {
        if (!Gear::Addr::textResources_instance || !Gear::Addr::textResources_get || !Gear::Addr::cUI_Control_getRect)
        {
            return;
        }
        __try
        {
            const wchar_t* const* entry = Gear::Addr::textResources_get(Gear::Addr::textResources_instance(), kMaximumText);
            const wchar_t* text = entry ? *entry : nullptr;
            auto width = fontSlot<TextWidthFn>(font, 0x2C);
            int32_t box[3] = {};
            Gear::Addr::cUI_Control_getRect(reinterpret_cast<void*>(tab + kResultControl), box);
            if (text && *text)
            {
                const int textWidth = width ? width(font, nullptr, text) : 0;
                const int boxWidth = static_cast<int16_t>(box[2] & 0xFFFF), boxHeight = static_cast<int16_t>((box[2] >> 16) & 0xFFFF);
                drawText(device, font, box[0] + (boxWidth - textWidth) / 2, box[1] + boxHeight, text, kRed);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The page's two extra texts after the engine drew the page (ReBorn draws them from inside the render function: its appended code at DE 01AC8087 and
    // 01AC7E1D): the chosen art's name and level on the left, and the red "maximum level" message at the result box.
    void drawPageTexts(uintptr_t tab, void* device)
    {
        const bool describe = g_describeActive.load(std::memory_order_relaxed), maximum = g_maximumActive.load(std::memory_order_relaxed);
        if (!describe && !maximum)
        {
            return;
        }
        uint32_t shown = 0, chosen = 0;
        read(tab, kShown, shown);
        read(tab, kChosen, chosen);
        const int rune = static_cast<int>(shown ? shown : chosen);
        const int ability = abilityOf(rune);
        void* font = ability ? fontById(kTextFont) : nullptr;
        if (!font)
        {
            return;
        }
        double level = 0, flag = 0;
        if (describe && LuaHost::masterEvent("master.describe", rune, ability, runesInSlots(tab), level) == LuaHost::Answer::Value && level >= 0 &&
            level <= 0xFFFF)
        {
            std::wstring text;
            if (describeAbility(ability, static_cast<int>(level), text))
            {
                drawText(device, font, kDescribeX, kDescribeY, text.c_str(), kWhite);
            }
        }
        if (maximum && LuaHost::masterEvent("master.maximum", rune, ability, runesInSlots(tab), flag) == LuaHost::Answer::Value && flag != 0)
        {
            drawMaximum(tab, device, font);
        }
    }


    // The engine thread draws the tab every frame while the window is open: the price is refreshed before it draws and before the next click
    // reads it. (The tab's event handler reads the entry the last frame wrote.)
    void __fastcall hookRender(void* tab, void* edx, void* device)
    {
        uint32_t flags = 0;
        read(reinterpret_cast<uintptr_t>(tab), kFlags, flags);
        if (g_priceActive.load(std::memory_order_relaxed) && (flags & 1))
        {
            refreshPrice(reinterpret_cast<uintptr_t>(tab));
        }
        g_origRender(tab, edx, device);
        if (flags & 1)
        {
            drawPageTexts(reinterpret_cast<uintptr_t>(tab), device);
        }
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
    else if (std::strcmp(event, "master.describe") == 0)
    {
        g_describeActive.store(active);
    }
    else if (std::strcmp(event, "master.maximum") == 0)
    {
        g_maximumActive.store(active);
    }
}
