#include "itemtip.h"
#include "game/gear_addr.h"
#include "inventory.h"
#include "log.h"
#include "world.h"

#include <windows.h>

namespace
{
    using namespace Gear;

    constexpr uintptr_t kReservedPopup = 0x54;                   // u16 in the UI manager: index of a spare popup
    constexpr uintptr_t kPopupX = 0x24, kPopupY = 0x28;          // its top-left corner (the flags below)
    constexpr uintptr_t kPopupW = 0x2C, kPopupH = 0x2E;          // its size as of the last layout (int16)
    constexpr uint32_t kPopupFlags = 0xA;                        // what the equipment hover sets (clamp to the screen)
    constexpr uint32_t kTextFlags = 0x5F;                        // the fourth argument of the item's text function
    constexpr size_t kTextFunction = 17;                         // item vtable slot that builds the tooltip text

    // 1 shown, 0 not possible (no manager or popup), -1 fault.
    int callShow(uintptr_t item, int x, int y, int* width, int* height)
    {
        __try
        {
            uint8_t* manager = Addr::g_pUiManager ? *Addr::g_pUiManager.get() : nullptr;
            if (!manager)
            {
                return 0;
            }
            void* popup = Addr::cUI_Manager_getPopup(manager, *reinterpret_cast<uint16_t*>(manager + kReservedPopup));
            if (!popup)
            {
                return 0;
            }
            *width = *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(popup) + kPopupW);
            *height = *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(popup) + kPopupH);
            // The text: a vector of wchar_t (begin, end, capacity) on the engine's allocator, as the engine makes it.
            uint32_t text[3] = {};
            auto* buffer = static_cast<uint16_t*>(Addr::engine_alloc(0x10));
            if (!buffer)
            {
                return 0;
            }
            buffer[0] = 0;
            text[0] = text[1] = reinterpret_cast<uint32_t>(buffer);
            text[2] = text[0] + 0x10;
            void** vtable = *reinterpret_cast<void***>(item);
            using TextFn = void(__fastcall*)(void* self, void* edx, void* out, void* record, uint32_t a, uint32_t flags);
            reinterpret_cast<TextFn>(vtable[kTextFunction])(reinterpret_cast<void*>(item), nullptr, text, nullptr, 0,
                                                              kTextFlags);
            Addr::cUI_Popup_addFlags(popup, kPopupFlags);
            *reinterpret_cast<int*>(static_cast<uint8_t*>(popup) + kPopupX) = x;
            *reinterpret_cast<int*>(static_cast<uint8_t*>(popup) + kPopupY) = y;
            Addr::cUI_Popup_setText()(popup, 0, text, 0);
            Addr::cUI_Popup_show(popup, 1);
            Addr::cUI_Popup_setOwner(popup, 0);
            Addr::engine_free(nullptr, reinterpret_cast<void*>(text[0]), (text[1] - text[0]) >> 1);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    bool available()
    {
        return Addr::g_pUiManager && Addr::cUI_Manager_getPopup && Addr::cUI_Popup_addFlags && Addr::cUI_Popup_setTextBody &&
               Addr::cUI_Popup_show && Addr::cUI_Popup_setOwner && Addr::engine_alloc && Addr::engine_free;
    }
}

bool ItemTip::show(uint32_t id, int x, int y, int& width, int& height, const char*& error)
{
    const uintptr_t item = available() ? World::itemObject(id) : 0;
    if (!item)
    {
        error = available() ? "no such item" : "the tooltip functions were not found";
        return false;
    }
    const int r = callShow(item, x, y, &width, &height);
    if (r < 0)
    {
        LOG("ItemTip: showing the tooltip of item {} faulted", id);
        error = "the game faulted while building the tooltip";
        return false;
    }
    if (!r)
    {
        error = "no tooltip popup is available now";
        return false;
    }
    return true;
}

bool ItemTip::showExported(const std::string& bytes, int x, int y, int& width, int& height, const char*& error)
{
    Inventory::LocalOnly localOnly;
    const uint32_t id = Inventory::importItem(bytes, error, true);
    if (!id)
    {
        return false;
    }
    const bool shown = show(id, x, y, width, height, error);
    const char* ignored = nullptr;
    Inventory::destroy(id, ignored);
    return shown;
}
