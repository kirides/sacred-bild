#include "game/ui_anchor.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config/ui.h"
#include "log.h"
#include "mem.h"
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

    using Mem::member;

    struct Anchor
    {
        uintptr_t window;       // UiManager member
        Config::Ui::Position Config::Ui::*position;   // [UI.Layout]
        bool render2;           // draws through UiWindowSlot::render2 (device, ?, ?)
        bool confine = true;    // clip to the frame's 1024x768 rect
    };

    // The in-game windows placed by [UI.Layout]. Defaults: the corners and edges they have in the 1024x768 layout;
    // stats and equipment form one column (656,0 .. 912,644) next to the minimap and share its corner.
    // The taskbar is not confined: its level-up button moves to the stats window's place (moveLevelUpButton).
    const Anchor kAnchors[] = {
        {UiManager::taskbar, &Config::Ui::taskbar, false, false},
        {UiManager::console, &Config::Ui::chat, false},
        {UiManager::inventory, &Config::Ui::inventory, false},
        {UiManager::minimap, &Config::Ui::minimap, false},
        {UiManager::stats, &Config::Ui::stats, false},
        {UiManager::equipment, &Config::Ui::equipment, false},
        {UiManager::netPortraits, &Config::Ui::portraits, true},
        {UiManager::blacksmith, &Config::Ui::shops, true},
        {UiManager::merchant, &Config::Ui::shops, true},
        {UiManager::master, &Config::Ui::shops, false},
        {UiManager::chest, &Config::Ui::shops, false},
        {UiManager::cube, &Config::Ui::shops, false},
        {UiManager::trade, &Config::Ui::shops, false},
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

    // Popups take the frame of the code that set their text (those set in the canvas are not listed) and are drawn
    // unconfined: their layout keeps them on the whole screen instead of the 1024x768 rect.
    void* g_popupVtable = nullptr;
    struct PopupFrame
    {
        void* popup;
        Frame frame;
    };
    PopupFrame g_popups[64];
    size_t g_popupCount = 0;
    SRWLOCK g_popupLock = SRWLOCK_INIT;

    // The help screen's entries (cUI_Manager_showHelp, per screen in the order of UiManager::helpPopups) take the
    // frame of the window they explain; 0 leaves an entry in the canvas (hints about the game in general).
    constexpr uintptr_t kScreenCorner = 1;      // the screen's top-left corner
    const uintptr_t kHelpWindows[7][9] = {
        // 1: no window open
        {},
        // 2: inventory: the [H] hint at the top-left corner, stats (skills, active window, values), inventory
        // (tabs), stats, inventory (sort button), equipment, taskbar (weapon slot, combat art slot)
        {kScreenCorner, UiManager::stats, UiManager::stats, UiManager::inventory, UiManager::stats,
            UiManager::inventory, UiManager::equipment, UiManager::taskbar, UiManager::taskbar},
        // 3: blacksmith
        {0, UiManager::blacksmith, UiManager::blacksmith, UiManager::blacksmith},
        // 4: combo master: master, then the inventory's combo and combat art tabs
        {0, UiManager::master, UiManager::master, UiManager::master, UiManager::inventory, UiManager::inventory},
        // 5: merchant: merchant, inventory, the gold in the stats window
        {0, UiManager::merchant, UiManager::merchant, UiManager::inventory, UiManager::stats},
        // 6: world map (full screen, in the canvas)
        {},
        // 7: rune exchange at the combat art master
        {0, UiManager::master, UiManager::master, UiManager::master},
    };
    Frame g_cornerFrame;

    using CreateGameWindowsFn = void(__fastcall*)(void* manager);
    using ShowHelpFn = void(__fastcall*)(void* manager, void* edx, void* device, uint32_t screen);
    using SetTextFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c);
    using LayoutFn = void(__fastcall*)(void* window);
    CreateGameWindowsFn g_origCreateGameWindows = nullptr;
    ShowHelpFn g_origShowHelp = nullptr;
    SetTextFn g_origSetText = nullptr;
    SetTextFn g_origSetTextId = nullptr;
    LayoutFn g_origPopupLayout = nullptr;
    LayoutFn g_layoutChildren = nullptr;

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

    // The frame `self` runs in; `what` names it for UiTrace (the UiManager member, or 1 for a popup).
    bool frameOf(void* self, Frame& out, uintptr_t* what = nullptr)
    {
        if (void* manager = *reinterpret_cast<void**>(Addr::g_pUiManager))
        {
            for (size_t i = 0; i < std::size(kAnchors); ++i)
            {
                if (member<void*>(manager, kAnchors[i].window) == self)
                {
                    out = g_frames[i];
                    if (what) *what = kAnchors[i].window;
                    return true;
                }
            }
        }
        if (*static_cast<void**>(self) != g_popupVtable)
        {
            return false;
        }
        out = {0.0f, 0.0f, false};
        AcquireSRWLockShared(&g_popupLock);
        for (size_t i = 0; i < g_popupCount; ++i)
        {
            if (g_popups[i].popup == self)
            {
                out = g_popups[i].frame;
                break;
            }
        }
        ReleaseSRWLockShared(&g_popupLock);
        if (what) *what = 1;
        return true;
    }

    void traceRender(uintptr_t what, const char* slot)
    {
        if (UiCanvas::tracing())
        {
            UiCanvas::trace(what == 1 ? Fmt::format("popup {}", slot) : Fmt::format("window +{:x} {}", what, slot));
        }
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
        uintptr_t what = 0;
        if (!frameOf(self, frame, &what))
        {
            return orig(self, edx, device);
        }
        UiCanvas::FrameScope scope{frame};
        traceRender(what, "render");
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
        uintptr_t what = 0;
        if (!frameOf(self, frame, &what))
        {
            return orig(self, edx, device, a, b);
        }
        UiCanvas::FrameScope scope{frame};
        traceRender(what, "render2");
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

    const Frame* anchorFrame(uintptr_t window)
    {
        for (size_t i = 0; i < std::size(kAnchors); ++i)
        {
            if (kAnchors[i].window == window)
            {
                return &g_frames[i];
            }
        }
        return nullptr;
    }

    // The taskbar's level-up button lies on the stats window's close button (both absolute 898, 10) and is meant to
    // be covered by it. Moved within the taskbar's frame to where the stats frame puts that spot, the taskbar draws,
    // highlights and hit-tests it there. Only a button still at its original position is moved (createGameWindows
    // runs again for every game).
    void moveLevelUpButton(void* manager)
    {
        void* taskbar = member<void*>(manager, UiManager::taskbar);
        const Frame* from = anchorFrame(UiManager::taskbar);
        const Frame* to = anchorFrame(UiManager::stats);
        if (!taskbar || !from || !to)
        {
            return;
        }
        void* button = static_cast<uint8_t*>(taskbar) + Taskbar::levelUpButton;
        auto& x = member<int>(button, UiControl::x);
        auto& y = member<int>(button, UiControl::y);
        if (x != Taskbar::levelUpX || y != Taskbar::levelUpY)
        {
            return;
        }
        x += static_cast<int>(std::lround(to->x - from->x));
        y += static_cast<int>(std::lround(to->y - from->y));
        LOG("UI anchor: taskbar level-up button moved to {},{} in the taskbar's frame", x, y);
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
        moveLevelUpButton(manager);
        // The popups exist since the manager's constructor.
        for (auto** p = member<void**>(manager, UiManager::popupsBegin); p != member<void**>(manager, UiManager::popupsEnd); ++p)
        {
            if (*p)
            {
                g_popupVtable = *static_cast<void**>(*p);
                wrap(*p, false);
                break;
            }
        }
    }

    // Popups may reach past their window's 1024x768 rect: the frame is kept unconfined.
    void setPopupFrame(void* popup, const Frame& frame)
    {
        const bool canvas = frame.x == 0.0f && frame.y == 0.0f;
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
            g_popups[i] = {popup, {frame.x, frame.y, false}};
            g_popupCount = std::max(g_popupCount, i + 1);
        }
        ReleaseSRWLockExclusive(&g_popupLock);
    }

    void notePopup(void* popup)
    {
        if (UiCanvas::tracingPopups())
        {
            UiCanvas::trace(Fmt::format("popup {} text set", popup));
        }
        setPopupFrame(popup, UiCanvas::frame());
    }

    // Runs in the canvas (the manager's own drawing), so the texts it sets leave its popups there; the entries that
    // explain a window move to that window's frame.
    void __fastcall hookShowHelp(void* manager, void* edx, void* device, uint32_t screen)
    {
        const bool help = member<uint32_t>(manager, UiManager::flags) & UiManager::helpScreen;
        g_origShowHelp(manager, edx, device, screen);
        const size_t index = static_cast<size_t>(static_cast<int16_t>(screen) - 1);
        if (!help || index >= std::size(kHelpWindows))
        {
            return;
        }
        void** popups = member<void**>(manager, UiManager::popupsBegin);
        const size_t count = member<void**>(manager, UiManager::popupsEnd) - popups;
        for (size_t i = 0; i < std::size(kHelpWindows[index]); ++i)
        {
            const uintptr_t window = kHelpWindows[index][i];
            const uint16_t slot = member<uint16_t>(manager, UiManager::helpPopups + 2 * i);
            const Frame* frame = window == kScreenCorner ? &g_cornerFrame : anchorFrame(window);
            if (window && frame && slot < count && popups[slot])
            {
                setPopupFrame(popups[slot], *frame);
            }
        }
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

    // The game keeps popups inside the 1024x768 screen (or centers them on it); keep them inside the whole screen.
    // The layout runs in the popup's render, in the popup's (unconfined) frame; popups drawn before the game windows
    // exist (menus) are still confined to the canvas and keep the game's layout.
    void __fastcall hookPopupLayout(void* popup)
    {
        if (UiCanvas::frame().confine)
        {
            g_origPopupLayout(popup);
            return;
        }
        auto& flags = member<uint32_t>(popup, Popup::flags);
        const uint32_t own = Popup::clampToScreen | Popup::centerOnScreen | Popup::keepChildren;
        const uint32_t saved = flags;
        flags = (saved & ~(Popup::clampToScreen | Popup::centerOnScreen)) | Popup::keepChildren;
        g_origPopupLayout(popup);
        auto& x = member<int>(popup, UiControl::x);
        auto& y = member<int>(popup, UiControl::y);
        const int w = member<int16_t>(popup, UiControl::width), h = member<int16_t>(popup, UiControl::height);
        const UiCanvas::Bounds screen = UiCanvas::screenBounds();
        const int left = static_cast<int>(std::ceil(screen.left)), top = static_cast<int>(std::ceil(screen.top));
        const int right = static_cast<int>(std::floor(screen.right)), bottom = static_cast<int>(std::floor(screen.bottom));
        if (saved & Popup::clampToScreen)
        {
            // Where the game would push the popup against an edge of its 1024x768 screen (the effects list is
            // placed at 16,16 to sit in the top-left corner), it goes against that edge of the real screen.
            if (x < 16) x = left + 16;
            else if (x + w + 16 > 1024) x = right - 32 - w;
            if (y < 16) y = top + 16;
            else if (y + h + 16 > 768) y = bottom - 32 - h;
            x = std::max(std::min(x, right - 32 - w), left + 16);
            y = std::max(std::min(y, bottom - 32 - h), top + 16);
        }
        if (saved & Popup::centerOnScreen)
        {
            x = (left + right) / 2 - w / 2;
            y = (top + bottom) / 2 - h / 2;
        }
        flags = (flags & ~own) | (saved & own);
        if (UiCanvas::tracingPopups())
        {
            UiCanvas::trace(Fmt::format("popup {} laid out at {},{} size {}x{} flags {:x}", popup, x, y, w, h, saved));
        }
        if (!(saved & Popup::keepChildren))
        {
            g_layoutChildren(popup);
        }
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
    if (!Config::ui.anchor || !UiCanvas::enabled())
    {
        return;
    }
    const Frame left = UiCanvas::placed(0.0f, 0.0f), right = UiCanvas::placed(1.0f, 1.0f);
    if (std::fabs(left.x) < 0.5f && std::fabs(left.y) < 0.5f && std::fabs(right.x) < 0.5f && std::fabs(right.y) < 0.5f)
    {
        LOG("UI anchor: the canvas covers the screen, nothing to move");
        return;
    }
    for (size_t i = 0; i < std::size(kAnchors); ++i)
    {
        const Config::Ui::Position& pos = Config::ui.*kAnchors[i].position;
        g_frames[i] = UiCanvas::placed(pos.x / 4096.0f, pos.y / 4096.0f);
        g_frames[i].confine = kAnchors[i].confine;
    }
    g_cornerFrame = left;
    LOG("UI anchor: frames reach {:.1f},{:.1f} .. {:.1f},{:.1f} beyond the canvas", left.x, left.y, right.x, right.y);
    findCode();
    Patch::hook(g_origCreateGameWindows, Addr::cUI_Manager_createGameWindows, &hookCreateGameWindows,
        "cUI_Manager::createGameWindows");
    Patch::hook(g_origShowHelp, Addr::cUI_Manager_showHelp, &hookShowHelp, "cUI_Manager::showHelp");
    Patch::hook(g_origSetText, Addr::cUI_Popup_setText, &hookSetText, "cUI_Popup::setText");
    Patch::hook(g_origSetTextId, Addr::cUI_Popup_setTextId, &hookSetTextId, "cUI_Popup::setTextId");
    g_layoutChildren = reinterpret_cast<LayoutFn>(Addr::cUI_Window2_layoutChildren);
    Patch::hook(g_origPopupLayout, Addr::cUI_Popup_layout, &hookPopupLayout, "cUI_Popup::layout");
}

bool UiAnchor::frame(void* window, UiCanvas::Frame& out)
{
    return window && frameOf(window, out);
}
