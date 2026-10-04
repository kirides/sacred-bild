#include "game/ui_anchor.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>

namespace
{
    using namespace Sacred;
    using UiCanvas::Frame;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + offset);
    }

    enum Edge { Low = 0, Center = 1, High = 2 };     // left/top, center, right/bottom

    struct Anchor
    {
        uintptr_t window;       // UiManager member
        Edge horizontal, vertical;
        bool render2;           // draws through UiWindowSlot::render2 (device, ?, ?)
    };

    // Where each window sits in the 1024x768 layout. The shop windows (top-left) and the inventory (bottom-left)
    // are used together; each keeps its own corner.
    constexpr Anchor kAnchors[] = {
        {UiManager::taskbar, Center, High, false},
        {UiManager::console, Center, High, false},
        {UiManager::inventory, Low, High, false},
        {UiManager::equipment, High, High, false},
        {UiManager::minimap, High, Low, false},
        {UiManager::stats, High, Low, false},
        {UiManager::netPortraits, Low, Low, true},
        {UiManager::blacksmith, Low, Low, true},
        {UiManager::merchant, Low, Low, true},
        {UiManager::master, Low, Low, false},
        {UiManager::chest, Low, Low, false},
        {UiManager::cube, Low, Low, false},
        {UiManager::trade, Low, Low, false},
    };
    Frame g_frames[std::size(kAnchors)];

    // Wrapped vtables: the game's vtable slots point at the thunks below, which look up the original by the
    // object's vtable. A vtable is shared by all objects of its class; objects without a frame pass straight through.
    constexpr size_t kSlotCount = UiWindowSlot::render2 / 4 + 1;
    struct Vtable
    {
        void** table;
        void* original[kSlotCount];
    };
    Vtable g_vtables[32];
    std::atomic<size_t> g_vtableCount{0};
    std::mutex g_patchMutex;
    uintptr_t g_codeBegin = 0, g_codeEnd = 0;

    // Popups take the frame of the code that set their text; those set in the canvas are not listed.
    struct PopupFrame
    {
        void* popup;
        Frame frame;
    };
    PopupFrame g_popups[64];
    size_t g_popupCount = 0;
    SRWLOCK g_popupLock = SRWLOCK_INIT;

    using CreateGameWindowsFn = void(__fastcall*)(void* manager);
    using SetTextFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c);
    CreateGameWindowsFn g_origCreateGameWindows = nullptr;
    SetTextFn g_origSetText = nullptr;
    SetTextFn g_origSetTextId = nullptr;

    void* original(void* self, uintptr_t slot)
    {
        void** table = *static_cast<void***>(self);
        const size_t n = g_vtableCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_vtables[i].table == table)
            {
                return g_vtables[i].original[slot / 4];
            }
        }
        return nullptr;     // unreachable: only patched vtables lead here
    }

    bool frameOf(void* self, Frame& out)
    {
        if (void* manager = *reinterpret_cast<void**>(Addr::g_pUiManager))
        {
            for (size_t i = 0; i < std::size(kAnchors); ++i)
            {
                if (member<void*>(manager, kAnchors[i].window) == self)
                {
                    out = g_frames[i];
                    return true;
                }
            }
        }
        AcquireSRWLockShared(&g_popupLock);
        bool found = false;
        for (size_t i = 0; i < g_popupCount && !found; ++i)
        {
            if (g_popups[i].popup == self)
            {
                out = g_popups[i].frame;
                found = true;
            }
        }
        ReleaseSRWLockShared(&g_popupLock);
        return found;
    }

    // Coordinates of the calling thread's frame, seen from `to`.
    int shiftX(int x, const Frame& to) { return static_cast<int>(std::lround(x + UiCanvas::frame().x - to.x)); }
    int shiftY(int y, const Frame& to) { return static_cast<int>(std::lround(y + UiCanvas::frame().y - to.y)); }

    int* mouseCoords(void* event)
    {
        const uintptr_t vtable = event ? *static_cast<uintptr_t*>(event) : 0;
        if (vtable != Addr::cEventMouseDown_vtable && vtable != Addr::cEventMouseUp_vtable)
        {
            return nullptr;
        }
        return &member<int>(event, MouseEvent::x);
    }

    // thiscall targets are wrapped as fastcall with an unused EDX parameter.
    using EventFn = uint32_t(__fastcall*)(void* self, void* edx, void* event);
    using RenderFn = uint32_t(__fastcall*)(void* self, void* edx, void* device);
    using InsideFn = uint32_t(__fastcall*)(void* self, void* edx, int x, int y);
    using ShowFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t show);
    using Render2Fn = uint32_t(__fastcall*)(void* self, void* edx, void* device, uint32_t a, uint32_t b);

    uint32_t __fastcall thunkEvent(void* self, void* edx, void* event)
    {
        const auto orig = reinterpret_cast<EventFn>(original(self, UiWindowSlot::receiveEvent));
        Frame frame;
        if (!frameOf(self, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, event);
        }
        // The event travels on to other windows afterwards: shift its position for this one only.
        int* coords = mouseCoords(event);
        int saved[2] = {};
        if (coords)
        {
            saved[0] = coords[0];
            saved[1] = coords[1];
            coords[0] = shiftX(saved[0], frame);
            coords[1] = shiftY(saved[1], frame);
        }
        uint32_t result;
        {
            UiCanvas::FrameScope scope{frame};
            result = orig(self, edx, event);
        }
        if (coords)
        {
            coords[0] = saved[0];
            coords[1] = saved[1];
        }
        return result;
    }

    uint32_t __fastcall thunkRender(void* self, void* edx, void* device)
    {
        const auto orig = reinterpret_cast<RenderFn>(original(self, UiWindowSlot::render));
        Frame frame;
        if (!frameOf(self, frame))
        {
            return orig(self, edx, device);
        }
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, device);
    }

    uint32_t __fastcall thunkInside(void* self, void* edx, int x, int y)
    {
        const auto orig = reinterpret_cast<InsideFn>(original(self, UiWindowSlot::isInside));
        Frame frame;
        if (!frameOf(self, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, x, y);
        }
        const int fx = shiftX(x, frame), fy = shiftY(y, frame);
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, fx, fy);
    }

    uint32_t __fastcall thunkShow(void* self, void* edx, uint32_t show)
    {
        const auto orig = reinterpret_cast<ShowFn>(original(self, UiWindowSlot::show));
        Frame frame;
        if (!frameOf(self, frame))
        {
            return orig(self, edx, show);
        }
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, show);
    }

    uint32_t __fastcall thunkRender2(void* self, void* edx, void* device, uint32_t a, uint32_t b)
    {
        const auto orig = reinterpret_cast<Render2Fn>(original(self, UiWindowSlot::render2));
        Frame frame;
        if (!frameOf(self, frame))
        {
            return orig(self, edx, device, a, b);
        }
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, device, a, b);
    }

    bool isCode(const void* p)
    {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a >= g_codeBegin && a < g_codeEnd;
    }

    // Points the object's vtable slots at the thunks (once per vtable). A slot is only touched if it and every slot
    // before it hold code addresses: the slot after a vtable's last one is the next vtable's RTTI pointer (data).
    void wrap(void* object, bool render2)
    {
        void** table = *static_cast<void***>(object);
        std::lock_guard lock(g_patchMutex);
        const size_t n = g_vtableCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_vtables[i].table == table)
            {
                return;
            }
        }
        if (n == std::size(g_vtables))
        {
            LOG("UI anchor: no room to wrap vtable {}", static_cast<void*>(table));
            return;
        }
        Vtable& v = g_vtables[n];
        v.table = table;
        size_t slots = 0;
        while (slots < kSlotCount && isCode(table[slots]))
        {
            v.original[slots] = table[slots];
            ++slots;
        }
        struct Wrapped { uintptr_t slot; void* thunk; };
        const Wrapped wrapped[] = {
            {UiWindowSlot::receiveEvent, reinterpret_cast<void*>(&thunkEvent)},
            {UiWindowSlot::render, reinterpret_cast<void*>(&thunkRender)},
            {UiWindowSlot::isInside, reinterpret_cast<void*>(&thunkInside)},
            {UiWindowSlot::show, reinterpret_cast<void*>(&thunkShow)},
            {UiWindowSlot::render2, render2 ? reinterpret_cast<void*>(&thunkRender2) : nullptr},
        };
        // Publish the originals before any slot leads to a thunk.
        g_vtableCount.store(n + 1, std::memory_order_release);
        int count = 0;
        for (const Wrapped& w : wrapped)
        {
            if (w.thunk && w.slot / 4 < slots)
            {
                count += Patch::value(reinterpret_cast<uintptr_t>(&table[w.slot / 4]), w.thunk) ? 1 : 0;
            }
        }
        LOG("UI anchor: vtable {} ({} slots) wrapped in {} places", static_cast<void*>(table), slots, count);
    }

    void __fastcall hookCreateGameWindows(void* manager)
    {
        g_origCreateGameWindows(manager);
        for (const Anchor& a : kAnchors)
        {
            if (void* window = member<void*>(manager, a.window))
            {
                wrap(window, a.render2);
            }
        }
    }

    void notePopup(void* popup)
    {
        const Frame current = UiCanvas::frame();
        const bool canvas = current.x == 0.0f && current.y == 0.0f;
        if (!canvas)
        {
            wrap(popup, false);
        }
        AcquireSRWLockExclusive(&g_popupLock);
        size_t i = 0;
        while (i < g_popupCount && g_popups[i].popup != popup)
        {
            ++i;
        }
        if (canvas)
        {
            if (i < g_popupCount)
            {
                g_popups[i] = g_popups[--g_popupCount];
            }
        }
        else if (i < std::size(g_popups))
        {
            // Popups may reach past their window's 1024x768 rect.
            g_popups[i] = {popup, {current.x, current.y, false}};
            g_popupCount = std::max(g_popupCount, i + 1);
        }
        ReleaseSRWLockExclusive(&g_popupLock);
    }

    uint32_t __fastcall hookSetText(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        notePopup(self);
        return g_origSetText(self, edx, a, b, c);
    }

    uint32_t __fastcall hookSetTextId(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        notePopup(self);
        return g_origSetTextId(self, edx, a, b, c);
    }

    void findCode()
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                g_codeBegin = base + section->VirtualAddress;
                g_codeEnd = g_codeBegin + section->Misc.VirtualSize;
                return;
            }
        }
    }
}

void UiAnchor::install()
{
    if (!g_config.uiAnchor || !UiCanvas::enabled())
    {
        return;
    }
    const Frame left = UiCanvas::anchored(Low, Low), right = UiCanvas::anchored(High, High);
    if (std::fabs(left.x) < 0.5f && std::fabs(left.y) < 0.5f && std::fabs(right.x) < 0.5f && std::fabs(right.y) < 0.5f)
    {
        LOG("UI anchor: the canvas covers the screen, nothing to move");
        return;
    }
    for (size_t i = 0; i < std::size(kAnchors); ++i)
    {
        g_frames[i] = UiCanvas::anchored(kAnchors[i].horizontal, kAnchors[i].vertical);
    }
    LOG("UI anchor: frames reach {:.1f},{:.1f} .. {:.1f},{:.1f} beyond the canvas", left.x, left.y, right.x, right.y);
    findCode();
    Patch::hook(g_origCreateGameWindows, Addr::cUI_Manager_createGameWindows, &hookCreateGameWindows,
        "cUI_Manager::createGameWindows");
    Patch::hook(g_origSetText, Addr::cUI_Popup_setText, &hookSetText, "cUI_Popup::setText");
    Patch::hook(g_origSetTextId, Addr::cUI_Popup_setTextId, &hookSetTextId, "cUI_Popup::setTextId");
}
