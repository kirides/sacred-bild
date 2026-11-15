#include "game/ui_anchor.h"
#include "game/build.h"
#include "game/resolution.h"
#include "game/ui_canvas.h"
#include "game/ui_state.h"
#include "game/ui_windows.h"
#include "config/debug.h"
#include "config/ui.h"
#include "log.h"
#include "patch.h"
#include "sacred/mouse.h"
#include "sacred/ui.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
    using namespace Sacred;
    using UiCanvas::Frame;
    using W = cUI_Manager::Window;

    struct Anchor
    {
        W window;
        Config::Ui::Position Config::Ui::*position;   // [UI.Layout]
        bool confine = true;    // clip to the frame's 1024x768 rect
    };

    // The in-game windows placed by [UI.Layout]. Defaults: the corners and edges they have in the 1024x768 layout;
    // stats and equipment form one column (656,0 .. 912,644) next to the minimap and share its corner.
    // The taskbar is not confined: its level-up button moves to the stats window's place (moveLevelUpButton).
    const Anchor kAnchors[] = {
        {W::taskbar, &Config::Ui::taskbar, false},
        {W::console, &Config::Ui::chat},
        {W::inventory, &Config::Ui::inventory},
        {W::minimap, &Config::Ui::minimap},
        {W::stats, &Config::Ui::stats},
        {W::equipment, &Config::Ui::equipment},
        {W::netPortraits, &Config::Ui::portraits},
        {W::blacksmith, &Config::Ui::shops},
        {W::merchant, &Config::Ui::shops},
        {W::master, &Config::Ui::shops},
        {W::chest, &Config::Ui::shops},
        {W::cube, &Config::Ui::shops},
        {W::trade, &Config::Ui::shops},
    };
    Frame g_frames[std::size(kAnchors)];
    bool g_anchored = false;        // [UI.Layout] places the windows (else they stay in the canvas)
    bool g_handlersSet = false;

    // Windows moved at run time (SacredBild_UiSetWindowFrame): a frame of their own, wherever it is.
    struct MovedWindow
    {
        bool set = false;
        float x = 0.0f, y = 0.0f;
    };
    MovedWindow g_moved[static_cast<size_t>(W::count)];
    SRWLOCK g_moveLock = SRWLOCK_INIT;

    // Popups take the frame of the code that set their text (those set in the canvas are not listed) and are drawn
    // unconfined: their layout keeps them on the whole screen instead of the 1024x768 rect.
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

    // Plain fastcall (popup) in the exe: hooked without EDX.
    using LayoutFn = void(__fastcall*)(cUI_Popup* popup);
    decltype(Addr::cUI_Manager_showHelp)::Ptr g_origShowHelp = nullptr;
    decltype(Addr::cUI_Popup_setText)::Ptr g_origSetText = nullptr;
    decltype(Addr::cUI_Popup_setTextId)::Ptr g_origSetTextId = nullptr;
    LayoutFn g_origPopupLayout = nullptr;

    // ReBorn's windows carry the offset of the edge they are anchored to ((screen - 1024) * 0, 1/2 or 1) in their
    // position. The anchor is the one that puts the window's original 1024x768 position (x0, y0) inside the screen
    // it was laid out for, the centered one if several do (or none, for a window parked elsewhere).
    float reBornAnchor(int pos, int size, int room)
    {
        int found = -1;
        for (int i = 0; i < 3; ++i)
        {
            const float origin = pos - 0.5f * i * room;     // i = 0, 1, 2: anchor 0, 1/2, 1
            if (origin >= -8.0f && origin + size <= 1024.0f + 8.0f)
            {
                if (i == 1)
                {
                    return 0.5f;
                }
                if (found < 0)
                {
                    found = i;
                }
            }
        }
        return found < 0 ? 0.5f : 0.5f * found;
    }

    Frame reBornFrame(const cUI_Window2* window)
    {
        {
            static int last[32][4];
            int i = 31;
            for (int k = 0; k < static_cast<int>(W::count); ++k)
            {
                if (cUI_Manager::instance()->window(static_cast<W>(k)) == window)
                {
                    i = k;
                }
            }
            if (last[i][0] != window->x || last[i][1] != window->y || last[i][2] != window->width || last[i][3] != window->height)
            {
                last[i][0] = window->x; last[i][1] = window->y; last[i][2] = window->width; last[i][3] = window->height;
                LOG("UI anchor: ReBorn window {} at {},{} {}x{}", i == 31 ? "?" : UiState::name(static_cast<W>(i)), window->x, window->y, window->width, window->height);
            }
        }
        return UiCanvas::placed(reBornAnchor(window->x, window->width, Resolution::width() - 1024),
                                reBornAnchor(window->y, window->height, Resolution::height() - 768));
    }

    // The frame an object of the manager's runs in (UiWindows::Handlers::frame).
    bool frameOf(const UiWindows::Owner& owner, Frame& out)
    {
        if (owner.kind == UiWindows::Kind::Window)
        {
            // A window moved at run time (Sacred Gear's layout editing) first.
            if (owner.index >= 0 && owner.index < static_cast<int>(W::count))
            {
                AcquireSRWLockShared(&g_moveLock);
                const MovedWindow moved = g_moved[owner.index];
                ReleaseSRWLockShared(&g_moveLock);
                if (moved.set)
                {
                    out = {moved.x, moved.y, owner.index != static_cast<int>(W::taskbar) && !Sacred::reBorn()};
                    return true;
                }
            }
            for (size_t i = 0; i < std::size(kAnchors); ++i)
            {
                if (owner.is(kAnchors[i].window))
                {
                    out = Sacred::reBorn() && cUI_Manager::instance() && cUI_Manager::instance()->window(kAnchors[i].window) ? reBornFrame(cUI_Manager::instance()->window(kAnchors[i].window)) : g_frames[i];
                    return true;
                }
            }
            return false;
        }
        if (owner.kind != UiWindows::Kind::Popup)
        {
            return false;
        }
        out = {0.0f, 0.0f, false};
        AcquireSRWLockShared(&g_popupLock);
        for (size_t i = 0; i < g_popupCount; ++i)
        {
            if (g_popups[i].popup == owner.object)
            {
                out = g_popups[i].frame;
                break;
            }
        }
        ReleaseSRWLockShared(&g_popupLock);
        return true;
    }

    // A popup whose text a window set (so it has a frame of its own): the tooltips and hints of that window.
    bool isWindowPopup(const UiWindows::Owner& owner)
    {
        bool found = false;
        const cUI_Control2* self = owner.object;
        if (owner.kind == UiWindows::Kind::Popup)
        {
            AcquireSRWLockShared(&g_popupLock);
            for (size_t i = 0; i < g_popupCount && !found; ++i)
            {
                found = g_popups[i].popup == self;
            }
            ReleaseSRWLockShared(&g_popupLock);
        }
        return found;
    }

    // Coordinates of the calling thread's frame, seen from `to`.
    int shiftX(int x, const Frame& to) { return static_cast<int>(std::lround(x + UiCanvas::frame().x - to.x)); }
    int shiftY(int y, const Frame& to) { return static_cast<int>(std::lround(y + UiCanvas::frame().y - to.y)); }

    // An event for an object whose frame isn't the calling thread's.
    uint32_t onEvent(const UiWindows::Owner& owner, const Frame& frame, cEvent* event,
        uint32_t(__fastcall* orig)(cUI_Control2*, void*, cEvent*))
    {
        cUI_Control2* self = owner.object;
        void* edx = nullptr;
        const int what = owner.kind == UiWindows::Kind::Window ? owner.index : -1;
        // The event travels on to other windows afterwards: shift its position for this one only.
        cEventMouse* mouse = cEventMouse::of(event);
        if (mouse && isWindowPopup(owner))
        {
            // A window's tooltip or hint (the blacksmith's "Place the weapon or armor here ...") is drawn over the
            // window's own controls: it must not take their clicks, or an item can't be dropped under its text.
            return 0;
        }
        cMouse* cursor = cMouse::instance();
        const bool heldBefore = mouse && cursor->heldItem != nullptr;
        int saved[2] = {};
        if (mouse)
        {
            saved[0] = mouse->x;
            saved[1] = mouse->y;
            mouse->x = shiftX(saved[0], frame);
            mouse->y = shiftY(saved[1], frame);
            if (heldBefore && what == static_cast<int>(W::blacksmith) && cursor->cursorImage)
            {
                // The blacksmith drops the item into the grid cell under the click, while the grid highlights the
                // cells under the held item's top-left corner (the drawn cursor, a cell above the click): a click at
                // the highlighted spot missed. Drop where the highlight is.
                int left = 0, top = 0;
                cursor->cursorPos(left, top);
                mouse->x = shiftX(left, frame);
                mouse->y = shiftY(top, frame);
            }
        }
        uint32_t result;
        {
            UiCanvas::FrameScope scope{frame};
            result = orig(self, edx, event);
        }
        if (mouse && Config::debug.uiTrace)
        {
            // What the window got, next to what the drawn cursor and the held item say (the drop highlight reads
            // the cursor's top-left corner, the click the event's position).
            int left = 0, top = 0;
            if (cursor->cursorImage)
            {
                cursor->cursorPos(left, top);
            }
            LOG("UI trace: {} mouse {} event {},{} -> window {},{} cursor top-left {},{} (canvas) held before {}, after {} -> {}",
                UiWindows::describe(owner),
                *reinterpret_cast<uintptr_t*>(event) == Addr::cEventMouseDown_vtable ? "down" : "up", saved[0], saved[1],
                shiftX(saved[0], frame), shiftY(saved[1], frame), left, top, heldBefore, cursor->heldItem != nullptr, result);
        }
        if (mouse)
        {
            mouse->x = saved[0];
            mouse->y = saved[1];
        }
        return result;
    }

    uint32_t onInside(const UiWindows::Owner& owner, const Frame& frame, int x, int y,
        uint32_t(__fastcall* orig)(cUI_Control2*, void*, int, int))
    {
        if (isWindowPopup(owner))
        {
            return 0;       // see onEvent: the cursor over a window's hint is still over the window's controls
        }
        const int fx = shiftX(x, frame), fy = shiftY(y, frame);
        UiCanvas::FrameScope scope{frame};
        return orig(owner.object, nullptr, fx, fy);
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

    void onWindowsCreated(cUI_Manager* manager)
    {
        moveLevelUpButton(manager);
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
        // The screen the game laid the popup out for (ReBorn's is the real one).
        const int gameW = Sacred::reBorn() ? Resolution::width() : 1024;
        const int gameH = Sacred::reBorn() ? Resolution::height() : 768;
        const int left = static_cast<int>(std::ceil(screen.left)), top = static_cast<int>(std::ceil(screen.top));
        const int right = static_cast<int>(std::floor(screen.right)), bottom = static_cast<int>(std::floor(screen.bottom));
        if (saved & cUI_Popup::clampToScreen)
        {
            // Where the game would push the popup against an edge of its 1024x768 screen (the effects list is
            // placed at 16,16 to sit in the top-left corner), it goes against that edge of the real screen.
            if (x < 16) x = left + 16;
            else if (x + w + 16 > gameW) x = right - 32 - w;
            if (y < 16) y = top + 16;
            else if (y + h + 16 > gameH) y = bottom - 32 - h;
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
}

void UiAnchor::install()
{
    if (!UiCanvas::enabled())
    {
        return;
    }
    // The handlers run whenever the canvas is on, so windows moved at run time (SacredBild_UiSetWindowFrame) have a
    // frame even without the [UI.Layout] anchoring; the windows' frames stay the canvas then.
    const Frame left = UiCanvas::placed(0.0f, 0.0f), right = UiCanvas::placed(1.0f, 1.0f);
    const bool room = !(std::fabs(left.x) < 0.5f && std::fabs(left.y) < 0.5f && std::fabs(right.x) < 0.5f && std::fabs(right.y) < 0.5f);
    g_anchored = Config::ui.anchor && room;
    if (Config::ui.anchor && !room)
    {
        LOG("UI anchor: the canvas covers the screen, nothing to move");
    }
    for (size_t i = 0; i < std::size(kAnchors); ++i)
    {
        const Config::Ui::Position& pos = Config::ui.*kAnchors[i].position;
        g_frames[i] = g_anchored ? UiCanvas::placed(pos.x / 4096.0f, pos.y / 4096.0f) : Frame{};
        g_frames[i].confine = kAnchors[i].confine && !Sacred::reBorn();
    }
    g_cornerFrame = g_anchored ? left : Frame{};
    if (g_anchored)
    {
        LOG("UI anchor: frames reach {:.1f},{:.1f} .. {:.1f},{:.1f} beyond the canvas", left.x, left.y, right.x, right.y);
    }
    UiWindows::Handlers handlers;
    handlers.frame = &frameOf;
    handlers.receiveEvent = &onEvent;
    handlers.isInside = &onInside;
    handlers.windowsCreated = &onWindowsCreated;
    UiWindows::setHandlers(handlers);
    g_handlersSet = true;
    if (g_anchored)
    {
        Patch::hook(g_origShowHelp, Addr::cUI_Manager_showHelp, &hookShowHelp, "cUI_Manager::showHelp");
        Patch::hook(g_origSetText, Addr::cUI_Popup_setText, &hookSetText, "cUI_Popup::setText");
        Patch::hook(g_origSetTextId, Addr::cUI_Popup_setTextId, &hookSetTextId, "cUI_Popup::setTextId");
        Patch::hook(g_origPopupLayout, Addr::cUI_Popup_layout, &hookPopupLayout, "cUI_Popup::layout");
    }
}

bool UiAnchor::frame(cUI_Window2* window, UiCanvas::Frame& out)
{
    return window && frameOf(UiWindows::identify(window), out);
}

// For Sacred Gear (gear/ui.cpp), which looks these up with GetProcAddress and works without them: mod overlays drawn
// in a window's frame move with the window. Push enters `window`'s frame (a UI manager window; nullptr or a window
// that is not anchored: the canvas) on the calling thread, unconfined unless `confine`; returns 0 and pushes nothing
// while SacredBild's UI canvas is off. Pop leaves the innermost pushed frame. Bounds: the screen in the current
// frame's coordinates (left, top, right, bottom).
namespace
{
    thread_local std::vector<UiCanvas::FrameScope*> t_gearFrames;
}

extern "C" int __cdecl SacredBild_UiPushFrame(void* window, int confine)
{
    if (!UiCanvas::enabled())
    {
        return 0;
    }
    Frame frame;
    if (!UiAnchor::frame(static_cast<cUI_Window2*>(window), frame))
    {
        frame = {};
    }
    frame.confine = confine != 0;
    t_gearFrames.push_back(new UiCanvas::FrameScope(frame));
    return 1;
}

extern "C" void __cdecl SacredBild_UiPopFrame()
{
    if (!t_gearFrames.empty())
    {
        delete t_gearFrames.back();
        t_gearFrames.pop_back();
    }
}

// Where the calling thread's current frame lands on the screen: out[0] screen pixels per layout unit, out[1], out[2]
// the screen position of the frame's layout (0, 0). Sacred Gear rasterizes text for the screen's pixels with it.
extern "C" int __cdecl SacredBild_UiPlacement(float* out)
{
    if (!UiCanvas::enabled() || !out)
    {
        return 0;
    }
    const UiCanvas::Placement p = UiCanvas::placement();
    out[0] = p.scale;
    out[1] = p.originX;
    out[2] = p.originY;
    return 1;
}

extern "C" int __cdecl SacredBild_UiScreenBounds(float* bounds)
{
    if (!UiCanvas::enabled() || !bounds)
    {
        return 0;
    }
    const UiCanvas::Bounds b = UiCanvas::screenBounds();
    bounds[0] = b.left;
    bounds[1] = b.top;
    bounds[2] = b.right;
    bounds[3] = b.bottom;
    return 1;
}

// Moving the game's windows at run time (Sacred Gear's layout editing). `slot` is the window's index in the UI
// manager's table (cUI_Manager::Window). Set: the window runs in a frame of its own at (x, y) against the canvas, in
// layout units, confined to that frame's 1024x768 rect like an anchored window (set 0: back to its [UI.Layout]
// place). The windows the game draws alone (world map, savegame, options, character export) do not move. Get: the
// window's frame now. Both return 0 while SacredBild's UI canvas is off.
extern "C" int __cdecl SacredBild_UiSetWindowFrame(int slot, float x, float y, int set)
{
    if (!g_handlersSet || slot < 0 || slot >= static_cast<int>(W::count))
    {
        return 0;
    }
    const W w = static_cast<W>(slot);
    if (w == W::megamap || w == W::savegame || w == W::options || w == W::character)
    {
        return 0;
    }
    AcquireSRWLockExclusive(&g_moveLock);
    g_moved[slot] = {set != 0, x, y};
    ReleaseSRWLockExclusive(&g_moveLock);
    return 1;
}

extern "C" int __cdecl SacredBild_UiGetWindowFrame(int slot, float* x, float* y)
{
    if (!g_handlersSet || slot < 0 || slot >= static_cast<int>(W::count) || !x || !y)
    {
        return 0;
    }
    UiWindows::Owner owner;
    owner.kind = UiWindows::Kind::Window;
    owner.index = slot;
    Frame frame;
    if (!frameOf(owner, frame))
    {
        frame = {};
    }
    *x = frame.x;
    *y = frame.y;
    return 1;
}