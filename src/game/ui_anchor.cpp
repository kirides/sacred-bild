#include "game/ui_anchor.h"
#include "game/ui_canvas.h"
#include "config/ui.h"
#include "log.h"
#include "patch.h"
#include "sacred/ui.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

namespace
{
    using namespace Sacred;
    using UiCanvas::Frame;
    using W = cUI_Manager::Window;

    struct Anchor
    {
        W window;
        Config::Ui::Position Config::Ui::*position;   // [UI.Layout]
        bool render2;           // draws through cUI_Window2::Vtable::render2 (device, ?, ?)
        bool confine = true;    // clip to the frame's 1024x768 rect
    };

    // The in-game windows placed by [UI.Layout]. Defaults: the corners and edges they have in the 1024x768 layout;
    // stats and equipment form one column (656,0 .. 912,644) next to the minimap and share its corner.
    // The taskbar is not confined: its level-up button moves to the stats window's place (moveLevelUpButton).
    const Anchor kAnchors[] = {
        {W::taskbar, &Config::Ui::taskbar, false, false},
        {W::console, &Config::Ui::chat, false},
        {W::inventory, &Config::Ui::inventory, false},
        {W::minimap, &Config::Ui::minimap, false},
        {W::stats, &Config::Ui::stats, false},
        {W::equipment, &Config::Ui::equipment, false},
        {W::netPortraits, &Config::Ui::portraits, true},
        {W::blacksmith, &Config::Ui::shops, true},
        {W::merchant, &Config::Ui::shops, true},
        {W::master, &Config::Ui::shops, false},
        {W::chest, &Config::Ui::shops, false},
        {W::cube, &Config::Ui::shops, false},
        {W::trade, &Config::Ui::shops, false},
    };
    Frame g_frames[std::size(kAnchors)];

    // Wrapped vtables: the game's vtable slots point at the thunks below, which look up the original by the
    // object's vtable. A vtable is shared by all objects of its class; objects without a frame pass straight through.
    constexpr size_t kSlotCount = sizeof(cUI_Window2::Vtable) / sizeof(void*);
    struct Vtable
    {
        const cUI_Control2::Vtable* table;
        cUI_Window2::Vtable original;   // the slots up to the table's last (the rest zero)
    };
    Vtable g_vtables[32];
    std::atomic<size_t> g_vtableCount{0};
    std::mutex g_patchMutex;
    uintptr_t g_codeBegin = 0, g_codeEnd = 0;

    // Popups take the frame of the code that set their text (those set in the canvas are not listed) and are drawn
    // unconfined: their layout keeps them on the whole screen instead of the 1024x768 rect.
    const cUI_Control2::Vtable* g_popupVtable = nullptr;
    struct PopupFrame
    {
        const cUI_Control2* popup;
        Frame frame;
    };
    PopupFrame g_popups[64];
    size_t g_popupCount = 0;
    SRWLOCK g_popupLock = SRWLOCK_INIT;

    // Where the help screen's entries (cUI_Manager_showHelp, per screen in the order of cUI_Manager::helpPopups) go:
    // into the frame of the window they explain, or (the default) left in the canvas (hints about the game in
    // general).
    struct HelpPlace
    {
        int8_t value = 0;       // 0 the canvas, 1 the screen's top-left corner, 2 + a window
    };
    constexpr HelpPlace kScreenCorner{1};
    constexpr HelpPlace in(W window) { return {static_cast<int8_t>(2 + static_cast<int>(window))}; }
    const HelpPlace kHelpWindows[7][9] = {
        // 1: no window open
        {},
        // 2: inventory: the [H] hint at the top-left corner, stats (skills, active window, values), inventory
        // (tabs), stats, inventory (sort button), equipment, taskbar (weapon slot, combat art slot)
        {kScreenCorner, in(W::stats), in(W::stats), in(W::inventory), in(W::stats), in(W::inventory),
            in(W::equipment), in(W::taskbar), in(W::taskbar)},
        // 3: blacksmith
        {{}, in(W::blacksmith), in(W::blacksmith), in(W::blacksmith)},
        // 4: combo master: master, then the inventory's combo and combat art tabs
        {{}, in(W::master), in(W::master), in(W::master), in(W::inventory), in(W::inventory)},
        // 5: merchant: merchant, inventory, the gold in the stats window
        {{}, in(W::merchant), in(W::merchant), in(W::inventory), in(W::stats)},
        // 6: world map (full screen, in the canvas)
        {},
        // 7: rune exchange at the combat art master
        {{}, in(W::master), in(W::master), in(W::master)},
    };
    Frame g_cornerFrame;

    // Plain fastcall (manager) in the exe: hooked without EDX.
    using CreateGameWindowsFn = void(__fastcall*)(cUI_Manager* manager);
    using LayoutFn = void(__fastcall*)(cUI_Popup* popup);
    CreateGameWindowsFn g_origCreateGameWindows = nullptr;
    decltype(Addr::cUI_Manager_showHelp)::Ptr g_origShowHelp = nullptr;
    decltype(Addr::cUI_Popup_setText)::Ptr g_origSetText = nullptr;
    decltype(Addr::cUI_Popup_setTextId)::Ptr g_origSetTextId = nullptr;
    LayoutFn g_origPopupLayout = nullptr;

    const cUI_Window2::Vtable& original(const cUI_Control2* self)
    {
        const size_t n = g_vtableCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_vtables[i].table == self->vtable)
            {
                return g_vtables[i].original;
            }
        }
        static const cUI_Window2::Vtable none{};
        return none;    // unreachable: only patched vtables lead here
    }

    // The frame `self` runs in; `what` names it for UiTrace (the window, or -1 for a popup).
    bool frameOf(const cUI_Control2* self, Frame& out, int* what = nullptr)
    {
        if (cUI_Manager* manager = cUI_Manager::instance())
        {
            for (size_t i = 0; i < std::size(kAnchors); ++i)
            {
                if (manager->window(kAnchors[i].window) == self)
                {
                    out = g_frames[i];
                    if (what) *what = static_cast<int>(kAnchors[i].window);
                    return true;
                }
            }
        }
        if (self->vtable != g_popupVtable)
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
        if (what) *what = -1;
        return true;
    }

    void traceRender(int what, const char* slot)
    {
        if (UiCanvas::tracing())
        {
            UiCanvas::trace(what < 0 ? Fmt::format("popup {}", slot)
                                     : Fmt::format("window +{:x} {}", offsetof(cUI_Manager, windows) + 4 * what, slot));
        }
    }

    // Coordinates of the calling thread's frame, seen from `to`.
    int shiftX(int x, const Frame& to) { return static_cast<int>(std::lround(x + UiCanvas::frame().x - to.x)); }
    int shiftY(int y, const Frame& to) { return static_cast<int>(std::lround(y + UiCanvas::frame().y - to.y)); }

    uint32_t __fastcall thunkEvent(cUI_Control2* self, void* edx, cEvent* event)
    {
        const auto orig = original(self).receiveEvent;
        Frame frame;
        if (!frameOf(self, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, event);
        }
        // The event travels on to other windows afterwards: shift its position for this one only.
        cEventMouse* mouse = cEventMouse::of(event);
        int saved[2] = {};
        if (mouse)
        {
            saved[0] = mouse->x;
            saved[1] = mouse->y;
            mouse->x = shiftX(saved[0], frame);
            mouse->y = shiftY(saved[1], frame);
        }
        uint32_t result;
        {
            UiCanvas::FrameScope scope{frame};
            result = orig(self, edx, event);
        }
        if (mouse)
        {
            mouse->x = saved[0];
            mouse->y = saved[1];
        }
        return result;
    }

    uint32_t __fastcall thunkRender(cUI_Control2* self, void* edx, IDirect3DDevice7* device)
    {
        const auto orig = original(self).render;
        Frame frame;
        int what = 0;
        if (!frameOf(self, frame, &what))
        {
            return orig(self, edx, device);
        }
        UiCanvas::FrameScope scope{frame};
        traceRender(what, "render");
        return orig(self, edx, device);
    }

    uint32_t __fastcall thunkInside(cUI_Control2* self, void* edx, int x, int y)
    {
        const auto orig = original(self).isInside;
        Frame frame;
        if (!frameOf(self, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, x, y);
        }
        const int fx = shiftX(x, frame), fy = shiftY(y, frame);
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, fx, fy);
    }

    uint32_t __fastcall thunkShow(cUI_Window2* self, void* edx, uint32_t show)
    {
        const auto orig = original(self).show;
        Frame frame;
        if (!frameOf(self, frame))
        {
            return orig(self, edx, show);
        }
        UiCanvas::FrameScope scope{frame};
        return orig(self, edx, show);
    }

    uint32_t __fastcall thunkRender2(cUI_Window2* self, void* edx, IDirect3DDevice7* device, uint32_t a, uint32_t b)
    {
        const auto orig = original(self).render2;
        Frame frame;
        int what = 0;
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
    void wrap(cUI_Window2* object, bool render2)
    {
        const cUI_Control2::Vtable* table = object->vtable;
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
            LOG("UI anchor: no room to wrap vtable {}", static_cast<const void*>(table));
            return;
        }
        Vtable& v = g_vtables[n];
        v.table = table;
        const auto* slots = reinterpret_cast<void* const*>(table);
        size_t count = 0;
        while (count < kSlotCount && isCode(slots[count]))
        {
            ++count;
        }
        std::memcpy(&v.original, table, count * sizeof(void*));
        // Publish the originals before any slot leads to a thunk.
        g_vtableCount.store(n + 1, std::memory_order_release);
        auto* game = const_cast<cUI_Window2::Vtable*>(static_cast<const cUI_Window2::Vtable*>(table));
        int wrapped = 0;
        const auto patch = [&](auto& slot, decltype(+slot) thunk) {
            if (reinterpret_cast<uintptr_t>(&slot) - reinterpret_cast<uintptr_t>(game) < count * sizeof(void*))
            {
                wrapped += Patch::value(reinterpret_cast<uintptr_t>(&slot), thunk) ? 1 : 0;
            }
        };
        patch(game->receiveEvent, &thunkEvent);
        patch(game->render, &thunkRender);
        patch(game->isInside, &thunkInside);
        patch(game->show, &thunkShow);
        if (render2)
        {
            patch(game->render2, &thunkRender2);
        }
        LOG("UI anchor: vtable {} ({} slots) wrapped in {} places", static_cast<const void*>(table), count, wrapped);
    }

    const Frame* anchorFrame(W window)
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
    void moveLevelUpButton(cUI_Manager* manager)
    {
        cUI_Taskbar2* taskbar = manager->taskbar();
        const Frame* from = anchorFrame(W::taskbar);
        const Frame* to = anchorFrame(W::stats);
        if (!taskbar || !from || !to)
        {
            return;
        }
        cUI_Button2& button = taskbar->levelUpButton;
        if (button.x != cUI_Taskbar2::levelUpX || button.y != cUI_Taskbar2::levelUpY)
        {
            return;
        }
        button.x += static_cast<int>(std::lround(to->x - from->x));
        button.y += static_cast<int>(std::lround(to->y - from->y));
        LOG("UI anchor: taskbar level-up button moved to {},{} in the taskbar's frame", button.x, button.y);
    }

    void __fastcall hookCreateGameWindows(cUI_Manager* manager)
    {
        g_origCreateGameWindows(manager);
        for (const Anchor& a : kAnchors)
        {
            if (cUI_Window2* window = manager->window(a.window))
            {
                wrap(window, a.render2);
            }
        }
        moveLevelUpButton(manager);
        // The popups exist since the manager's constructor.
        for (cUI_Popup* popup : manager->popups)
        {
            if (popup)
            {
                g_popupVtable = popup->vtable;
                wrap(popup, false);
                break;
            }
        }
    }

    // Popups may reach past their window's 1024x768 rect: the frame is kept unconfined.
    void setPopupFrame(const cUI_Popup* popup, const Frame& frame)
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

    void notePopup(const cUI_Popup* popup)
    {
        if (UiCanvas::tracingPopups())
        {
            UiCanvas::trace(Fmt::format("popup {} text set", static_cast<const void*>(popup)));
        }
        setPopupFrame(popup, UiCanvas::frame());
    }

    // Runs in the canvas (the manager's own drawing), so the texts it sets leave its popups there; the entries that
    // explain a window move to that window's frame.
    void __fastcall hookShowHelp(cUI_Manager* manager, void* edx, IDirect3DDevice7* device, uint32_t screen)
    {
        const bool help = manager->flags & cUI_Manager::helpScreen;
        g_origShowHelp(manager, edx, device, screen);
        const size_t index = static_cast<size_t>(static_cast<int16_t>(screen) - 1);
        if (!help || index >= std::size(kHelpWindows))
        {
            return;
        }
        const Vector<cUI_Popup*>& popups = manager->popups;
        for (size_t i = 0; i < std::size(kHelpWindows[index]); ++i)
        {
            const HelpPlace place = kHelpWindows[index][i];
            const uint16_t slot = manager->helpPopups[i];
            const Frame* frame = place.value == kScreenCorner.value ? &g_cornerFrame
                : place.value >= 2 ? anchorFrame(static_cast<W>(place.value - 2)) : nullptr;
            if (frame && slot < popups.size() && popups[slot])
            {
                setPopupFrame(popups[slot], *frame);
            }
        }
    }

    uint32_t __fastcall hookSetText(cUI_Popup* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        notePopup(self);
        return g_origSetText(self, edx, a, b, c);
    }

    uint32_t __fastcall hookSetTextId(cUI_Popup* self, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        notePopup(self);
        return g_origSetTextId(self, edx, a, b, c);
    }

    // The game keeps popups inside the 1024x768 screen (or centers them on it); keep them inside the whole screen.
    // The layout runs in the popup's render, in the popup's (unconfined) frame; popups drawn before the game windows
    // exist (menus) are still confined to the canvas and keep the game's layout.
    void __fastcall hookPopupLayout(cUI_Popup* popup)
    {
        if (UiCanvas::frame().confine)
        {
            g_origPopupLayout(popup);
            return;
        }
        uint32_t& flags = popup->popupFlags;
        const uint32_t own = cUI_Popup::clampToScreen | cUI_Popup::centerOnScreen | cUI_Popup::keepChildren;
        const uint32_t saved = flags;
        flags = (saved & ~(cUI_Popup::clampToScreen | cUI_Popup::centerOnScreen)) | cUI_Popup::keepChildren;
        g_origPopupLayout(popup);
        int& x = popup->x;
        int& y = popup->y;
        const int w = popup->width, h = popup->height;
        const UiCanvas::Bounds screen = UiCanvas::screenBounds();
        const int left = static_cast<int>(std::ceil(screen.left)), top = static_cast<int>(std::ceil(screen.top));
        const int right = static_cast<int>(std::floor(screen.right)), bottom = static_cast<int>(std::floor(screen.bottom));
        if (saved & cUI_Popup::clampToScreen)
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
        if (saved & cUI_Popup::centerOnScreen)
        {
            x = (left + right) / 2 - w / 2;
            y = (top + bottom) / 2 - h / 2;
        }
        flags = (flags & ~own) | (saved & own);
        if (UiCanvas::tracingPopups())
        {
            UiCanvas::trace(Fmt::format("popup {} laid out at {},{} size {}x{} flags {:x}", static_cast<const void*>(popup),
                x, y, w, h, saved));
        }
        if (!(saved & cUI_Popup::keepChildren))
        {
            popup->layoutChildren();
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
    Patch::hook(g_origPopupLayout, Addr::cUI_Popup_layout, &hookPopupLayout, "cUI_Popup::layout");
}

bool UiAnchor::frame(cUI_Window2* window, UiCanvas::Frame& out)
{
    return window && frameOf(window, out);
}
