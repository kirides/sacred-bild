#include "itemtip.h"
#include "game/gear_addr.h"
#include "inventory.h"
#include "log.h"
#include "patch.h"
#include "world.h"

#include <windows.h>

#include <cstring>

namespace
{
    using namespace Gear;

    // SacredBild's frame of a popup (0, 0 without SacredBild or for a popup in the canvas).
    bool popupFrame(const void* popup, float& x, float& y)
    {
        using Fn = int(__cdecl*)(const void*, float*, float*);
        static const Fn fn = [] {
            const HMODULE bild = GetModuleHandleW(L"ddraw.dll");
            return bild ? reinterpret_cast<Fn>(GetProcAddress(bild, "SacredBild_UiGetPopupFrame")) : nullptr;
        }();
        return fn && fn(popup, &x, &y) == 2;
    }

    uint32_t g_scratchShown = 0, g_fallbackUsed = 0;

    // The item whose tooltip the engine showed last: every item tooltip ties the popup to the item with setOwner (the hover code of the
    // inventory, chest, shop and equipment windows), a tooltip of Gear's own (`show`) with owner 0, which is not recorded.
    decltype(Addr::cUI_Popup_setOwner)::Ptr g_origSetOwner = nullptr;
    void* g_hoverPopup = nullptr;
    uint32_t g_hoverId = 0, g_hoverFrame = 0, g_frame = 0;

    void __fastcall hookSetOwner(void* popup, void* edx, uint32_t itemId)
    {
        g_origSetOwner(popup, edx, itemId);
        if (itemId)
        {
            g_hoverPopup = popup;
            g_hoverId = itemId;
            g_hoverFrame = g_frame;
        }
    }

    constexpr uintptr_t kReservedPopup = 0x54;                   // u16 in the UI manager: index of a spare popup
    constexpr uintptr_t kPopupX = 0x24, kPopupY = 0x28;          // its top-left corner (the flags below)
    constexpr uintptr_t kPopupW = 0x2C, kPopupH = 0x2E;          // its size as of the last layout (int16)
    constexpr uint32_t kPopupFlags = 0xA;                        // what the equipment hover sets (clamp to the screen)
    constexpr uint32_t kTextFlags = 0x5F;                        // the fourth argument of the item's text function
    constexpr size_t kTextFunction = 17;                         // item vtable slot that builds the tooltip text

    // Where to copy the text instead of showing a popup (tests, mods that want the words).
    struct Capture
    {
        wchar_t* buffer = nullptr;
        int capacity = 0;
        int length = 0;
    };

    // 1 shown (or captured), 0 not possible (no manager or popup), -1 fault.
    int callShow(uintptr_t item, int x, int y, int* width, int* height, Capture* capture = nullptr)
    {
        __try
        {
            uint8_t* manager = Addr::g_pUiManager ? *Addr::g_pUiManager.get() : nullptr;
            if (!manager)
            {
                return 0;
            }
            void* popup = capture ? nullptr : Addr::cUI_Manager_getPopup(manager, *reinterpret_cast<uint16_t*>(manager + kReservedPopup));
            if (!capture && !popup)
            {
                return 0;
            }
            if (popup)
            {
                *width = *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(popup) + kPopupW);
                *height = *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(popup) + kPopupH);
            }
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
            if (capture)
            {
                const int n = static_cast<int>((text[1] - text[0]) / 2);
                capture->length = n < capture->capacity ? n : capture->capacity;
                std::memcpy(capture->buffer, reinterpret_cast<void*>(text[0]), capture->length * 2);
            }
            else
            {
                Addr::cUI_Popup_addFlags(popup, kPopupFlags);
                *reinterpret_cast<int*>(static_cast<uint8_t*>(popup) + kPopupX) = x;
                *reinterpret_cast<int*>(static_cast<uint8_t*>(popup) + kPopupY) = y;
                Addr::cUI_Popup_setText()(popup, 0, text, 0);
                Addr::cUI_Popup_show(popup, 1);
                Addr::cUI_Popup_setOwner(popup, 0);
            }
            Addr::engine_free(nullptr, reinterpret_cast<void*>(text[0]), (text[1] - text[0]) >> 1);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // An item object the object factory constructs and nobody registers: no id, no sector list, no network. 1 made.
    int callFactory(uint32_t kind, uint32_t type, uintptr_t& object)
    {
        __try
        {
            object = reinterpret_cast<uintptr_t>(Addr::cObjectFactory_create(nullptr, kind, type, 0));
            return object ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The object's own virtual destructor with the delete flag (as the object manager takes down an object it made).
    int callDelete(uintptr_t object)
    {
        __try
        {
            void** vtable = *reinterpret_cast<void***>(object);
            using DeleteFn = void*(__fastcall*)(void* self, void* edx, uint32_t flags);
            reinterpret_cast<DeleteFn>(vtable[0])(reinterpret_cast<void*>(object), nullptr, 1);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The class of an item type: the type record's byte +0x2E (the factory's switch); 0 if unknown.
    uint32_t kindOfType(uint32_t type)
    {
        uint8_t* tm = Addr::g_pTypeManager ? *reinterpret_cast<uint8_t**>(Addr::g_pTypeManager.address) : nullptr;
        if (!tm || type == 0 || type >= 0x7E60)
        {
            return 0;
        }
        __try
        {
            return tm[type * 0x80 + 0x2E];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool available()
    {
        return Addr::g_pUiManager && Addr::cUI_Manager_getPopup && Addr::cUI_Popup_addFlags && Addr::cUI_Popup_setTextBody &&
               Addr::cUI_Popup_show && Addr::cUI_Popup_setOwner && Addr::engine_alloc && Addr::engine_free;
    }

    // Shows (or, with `capture`, reads) the tooltip of exported item bytes. Preferred: a scratch object the object factory
    // constructs and nobody registers (no id, no object manager, no sector list, no network); if that is not possible, a
    // registered item made local-only.
    bool forExported(const std::string& bytes, int x, int y, int& width, int& height, Capture* capture, const char*& error)
    {
        if (Addr::cObjectFactory_create && available())
        {
            const uint32_t type = Inventory::exportedType(bytes);
            const uint32_t kind = kindOfType(type);
            uintptr_t object = 0;
            if (kind && callFactory(kind, type, object) > 0)
            {
                const char* fillError = nullptr;
                bool done = false;
                if (Inventory::fillObject(object, bytes, fillError))
                {
                    const int r = callShow(object, x, y, &width, &height, capture);
                    done = r > 0;
                    if (r < 0)
                    {
                        LOG("ItemTip: the tooltip of a scratch item (type {}) faulted", type);
                    }
                }
                callDelete(object);
                if (done)
                {
                    if (++g_scratchShown == 1 || g_scratchShown % 500 == 0)
                    {
                        LOG("ItemTip: {} tooltips from scratch items (no object manager)", g_scratchShown);
                    }
                    return true;
                }
            }
        }
        if (++g_fallbackUsed == 1 || g_fallbackUsed % 500 == 0)
        {
            LOG("ItemTip: {} tooltips through a local-only registered item (the scratch path did not work)", g_fallbackUsed);
        }
        Inventory::LocalOnly localOnly;
        const uint32_t id = Inventory::importItem(bytes, error, true);
        if (!id)
        {
            return false;
        }
        const uintptr_t item = World::itemObject(id);
        const bool done = item && callShow(item, x, y, &width, &height, capture) > 0;
        const char* ignored = nullptr;
        Inventory::destroy(id, ignored);
        if (!done)
        {
            error = "no tooltip popup is available now";
        }
        return done;
    }

    bool forItem(uint32_t id, int x, int y, int& width, int& height, Capture* capture, const char*& error)
    {
        const uintptr_t item = available() ? World::itemObject(id) : 0;
        if (!item)
        {
            error = available() ? "no such item" : "the tooltip functions were not found";
            return false;
        }
        const int r = callShow(item, x, y, &width, &height, capture);
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
}

bool ItemTip::show(uint32_t id, int x, int y, int& width, int& height, const char*& error)
{
    return forItem(id, x, y, width, height, nullptr, error);
}

bool ItemTip::showExported(const std::string& bytes, int x, int y, int& width, int& height, const char*& error)
{
    return forExported(bytes, x, y, width, height, nullptr, error);
}

bool ItemTip::text(uint32_t id, std::wstring& out, const char*& error)
{
    wchar_t buffer[4096];
    Capture capture{buffer, 4096, 0};
    int w = 0, h = 0;
    if (!forItem(id, 0, 0, w, h, &capture, error))
    {
        return false;
    }
    out.assign(buffer, capture.length);
    return true;
}

bool ItemTip::textForType(uint32_t type, std::wstring& out, const char*& error)
{
    if (!Addr::cObjectFactory_create || !available())
    {
        error = "the tooltip functions were not found";
        return false;
    }
    const uint32_t kind = kindOfType(type);
    uintptr_t object = 0;
    if (!kind || callFactory(kind, type, object) <= 0 || !object)
    {
        error = "no item of that type can be made";
        return false;
    }
    wchar_t buffer[4096];
    Capture capture{buffer, 4096, 0};
    int w = 0, h = 0;
    const int r = callShow(object, 0, 0, &w, &h, &capture);
    callDelete(object);
    if (r <= 0)
    {
        error = r < 0 ? "the game faulted while building the tooltip" : "no tooltip popup is available now";
        return false;
    }
    out.assign(buffer, capture.length);
    return true;
}

bool ItemTip::textExported(const std::string& bytes, std::wstring& out, const char*& error)
{
    wchar_t buffer[4096];
    Capture capture{buffer, 4096, 0};
    int w = 0, h = 0;
    if (!forExported(bytes, 0, 0, w, h, &capture, error))
    {
        return false;
    }
    out.assign(buffer, capture.length);
    return true;
}

bool ItemTip::hovered(Hover& out)
{
    // The engine shows an item's tooltip during the window render of a frame, after the tick: shown in frame N, the tick of frame N + 1
    // sees it one frame old; two frames old means the cursor has left the item.
    if (!g_hoverPopup || !g_hoverId || g_frame - g_hoverFrame > 1)
    {
        return false;
    }
    __try
    {
        const auto* popup = static_cast<const uint8_t*>(g_hoverPopup);
        out.id = g_hoverId;
        out.x = *reinterpret_cast<const int*>(popup + kPopupX);
        out.y = *reinterpret_cast<const int*>(popup + kPopupY);
        out.width = *reinterpret_cast<const int16_t*>(popup + kPopupW);
        out.height = *reinterpret_cast<const int16_t*>(popup + kPopupH);
        out.flags = *reinterpret_cast<const uint32_t*>(popup + 0x154);
        out.frameX = out.frameY = 0;
        out.framed = popupFrame(popup, out.frameX, out.frameY);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void ItemTip::frame()
{
    ++g_frame;
}

bool ItemTip::install()
{
    if (!Addr::cUI_Popup_setOwner)
    {
        LOG("ItemTip: the popup's owner setter was not found: no hovered item");
        return false;
    }
    return Patch::hook(g_origSetOwner, Addr::cUI_Popup_setOwner, hookSetOwner, "cUI_Popup_setOwner");
}

int ItemTip::equipSlot(uint32_t creatureId, uint32_t itemType)
{
    const uintptr_t hero = Addr::cCreature_equipSlot ? World::objectAddress(creatureId) : 0;
    if (!hero || itemType == 0 || itemType >= 0x7E60)
    {
        return -1;
    }
    __try
    {
        // The engine's answer depends on what is worn (the first free slot of a group): ask about a copy without equipment.
        uint8_t copy[0x600];
        std::memcpy(copy, reinterpret_cast<void*>(hero), sizeof(copy));
        std::memset(copy + 0x1A4, 0, 19 * 4);
        const uint32_t slot = Addr::cCreature_equipSlot(copy, itemType);
        return slot < 19 ? static_cast<int>(slot) : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}
