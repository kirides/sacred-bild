#include "game/ui_canvas.h"
#include "game/build.h"
#include "game/controller.h"
#include "game/device_proxy.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "game/ui_state.h"
#include "config/debug.h"
#include "config/ui.h"
#include "log.h"
#include "patch.h"
#include "sacred/mouse.h"
#include "sacred/ui.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
    using namespace Sacred;

    bool g_enabled = false;

    // Canvas placement in physical pixels: in game, and in the menus (ScaleMode=InGame: always as large as fits).
    struct Layout
    {
        float scale = 1.0f;
        float left = 0.0f;
        float top = 0.0f;
    };
    Layout g_game, g_menu;
    Layout g_startMenu;     // ReBorn: the start menu, whose entries carry the center offset like its in-game windows
    thread_local int t_depth = 0;
    thread_local const Layout* t_layout = nullptr;  // taken by the outermost UI scope for all of its draws
    thread_local UiCanvas::Frame t_frame;
    thread_local UiCanvas::Frame t_outerFrame;     // the frame before the outermost UI scope
    thread_local bool t_trace = false;
    std::atomic<ULONGLONG> g_tracePopupsUntil{0};
    bool g_traceKeyDown = false;
    std::atomic<int> g_autoTrace{0};     // UI frames to trace without Scroll Lock (ReBorn: after a menu screen change)

    decltype(Addr::getClientCursorPos)::Ptr g_origGetClientCursorPos = nullptr;
    decltype(Addr::cMouse_renderCursor)::Ptr g_origRenderCursor = nullptr;
    decltype(Addr::cEngine_worldMouse)::Ptr g_origWorldMouse = nullptr;
    decltype(Addr::cEngine_receiveEvent)::Ptr g_origReceiveEvent = nullptr;
    decltype(Addr::renderSavePortrait)::Ptr g_origSavePortrait = nullptr;
    decltype(Addr::cInventoryEntry_render)::Ptr g_origHeldItem = nullptr;

    // ReBorn's menu screens: the start menu has the center offset of its entries; a screen bigger than 1024x768 is
    // laid out for the real screen (the character selection: a 3D scene over the whole screen) and drawn and picked
    // as it is; the others are 1024x768 at the top left.
    const Layout& reBornMenuLayout(const cUI_Manager& manager)
    {
        static const Layout native{1.0f, 0.0f, 0.0f};
        const Layout* chosen = &g_menu;
        bool start = false;
        int shown = -1;
        for (int i = 0; i < cUI_Manager::menuCount; ++i)
        {
            const cUI_Window2* menu = manager.menus[i];
            if (!menu || !menu->visible())
            {
                continue;
            }
            shown = shown < 0 ? i : shown;
            if (i == 0)
            {
                start = true;
            }
            else if (menu->width > 1024 || menu->height > 768)
            {
                chosen = &native;
                break;
            }
        }
        if (chosen == &g_menu && start)
        {
            chosen = &g_startMenu;
        }
        static const Layout* last = nullptr;
        if (chosen != last)
        {
            last = chosen;
            if (Config::debug.uiTrace)
            {
                g_autoTrace = 3;
            }
            LOG("UI canvas: menu layout {} (first visible screen {})", chosen == &native ? "native" : chosen == &g_startMenu ? "start menu" : "canvas", shown);
            for (int i = 0; i < cUI_Manager::menuCount; ++i)
            {
                const cUI_Window2* menu = manager.menus[i];
                if (menu && menu->visible())
                {
                    LOG("UI canvas: menu {} visible at {},{} {}x{}", i, menu->x, menu->y, menu->width, menu->height);
                }
            }
        }
        return *chosen;
    }

    // Sacred Gear's cursor filter (SacredBild_UiSetCursorFilter) and its answer for the current UI frame.
    using CursorFilterFn = int(__cdecl*)(int x, int y);
    std::atomic<CursorFilterFn> g_cursorFilter{nullptr};
    std::atomic<bool> g_cursorHidden{false};
    thread_local bool t_realCursor = false;    // the held item follows the real cursor
    constexpr int kHiddenCursor = -30000;      // far off every window, still a short

    bool cursorHidden() { return !t_realCursor && g_cursorHidden.load(std::memory_order_relaxed); }

    bool modUiAt(int x, int y)
    {
        const CursorFilterFn filter = g_cursorFilter.load(std::memory_order_acquire);
        return filter && filter(x, y) != 0;
    }

    // The UI manager's state picks the layout: in game, unless a window the manager draws alone (savegame, options,
    // character, world map) replaces the game's screen. A UI scope keeps the layout it started with.
    const Layout& liveLayout()
    {
        const cUI_Manager* manager = cUI_Manager::instance();
        const bool inGame = manager && (manager->flags & cUI_Manager::inGame) && !UiState::exclusiveWindow();
        if (Sacred::reBorn())
        {
            static bool lastInv = false;
            const bool inv = manager && UiState::shown(UiState::Window::inventory);
            if (inv != lastInv)
            {
                lastInv = inv;
                if (inv && Config::debug.uiTrace) { g_autoTrace = 3; }
            }
            static const cUI_Window2* lastExclusive = nullptr;
            const cUI_Window2* ex = manager ? UiState::exclusiveWindow() : nullptr;
            if (ex != lastExclusive)
            {
                lastExclusive = ex;
                if (ex)
                {
                    LOG("UI canvas: exclusive window at {},{} {}x{}", ex->x, ex->y, ex->width, ex->height);
                }
                else
                {
                    LOG("UI canvas: no exclusive window");
                }
            }
        }
        if (!inGame && Sacred::reBorn() && manager && (manager->flags & cUI_Manager::inGame))
        {
            // ReBorn's world map is a window of the screen's size: drawn and picked as it is.
            const cUI_Window2* alone = UiState::exclusiveWindow();
            if (alone && (alone->width > 1024 || alone->height > 768))
            {
                static const Layout native{1.0f, 0.0f, 0.0f};
                return native;
            }
        }
        if (!inGame && Sacred::reBorn() && manager && (manager->flags & cUI_Manager::inMenus))
        {
            return reBornMenuLayout(*manager);
        }
        return inGame ? g_game : g_menu;
    }

    const Layout& layout() { return t_layout ? *t_layout : liveLayout(); }

    // The bytes before `ra` encode a call instruction.
    bool isCallSite(uintptr_t ra)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(ra);
        return p[-5] == 0xE8 ||                                             // call rel32
            (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) ||                    // call [disp32] / [reg+disp32]
            (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) ||                    // call [reg+disp8]
            (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) ||   // call reg / [reg]
            (p[-4] == 0xFF && p[-3] == 0x54);                               // call [sib+disp8]
    }

    // Return addresses into sacred.exe on the calling thread's stack, innermost first (stale ones included).
    std::string exeCallers()
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const uintptr_t lo = base + 0x1000, hi = base + nt->OptionalHeader.SizeOfImage;
        const auto* top = static_cast<const uintptr_t*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        std::string out;
        int found = 0;
        for (auto* p = static_cast<const uintptr_t*>(_AddressOfReturnAddress()); p < top && found < 16; ++p)
        {
            if (*p >= lo + 8 && *p < hi && isCallSite(*p))
            {
                out += Fmt::format(" {:08x}", *p);
                ++found;
            }
        }
        return out;
    }

    void notifyProxy()
    {
        if (g_enabled && t_depth > 0)
        {
            if (DeviceProxy* proxy = DeviceProxy::instance())
            {
                proxy->uiFrameChanged();
            }
        }
    }

    // The mouse's position, or the controller's cursor while it drives the game.
    void __cdecl hookGetClientCursorPos(HWND hwnd, POINT* pt)
    {
        POINT controller;
        if (pt && Controller::cursor(controller))
        {
            *pt = controller;
        }
        else
        {
            g_origGetClientCursorPos(hwnd, pt);
        }
        if (pt)
        {
            pt->x = UiCanvas::toVirtualX(pt->x);
            pt->y = UiCanvas::toVirtualY(pt->y);
        }
    }

    void __fastcall hookRenderCursor(cMouse* self, void* edx, IDirect3DDevice7* device, int flag)
    {
        if (Controller::hideGameCursor(reinterpret_cast<uintptr_t>(_ReturnAddress())))
        {
            return;
        }
        UiCanvas::Scope ui{UiCanvas::Mode::Overlay};
        UiCanvas::FrameScope canvas{{0.0f, 0.0f, false}};
        g_origRenderCursor(self, edx, device, flag);
    }

    // The item held by the cursor follows it over the whole screen, out of the frame of the window that draws it.
    void __fastcall hookHeldItem(void* self, void* edx, IDirect3DDevice7* device, int flag)
    {
        const UiCanvas::Frame current = UiCanvas::frame();
        UiCanvas::FrameScope unconfined{{current.x, current.y, false}};
        const bool real = t_realCursor;
        t_realCursor = true;
        g_origHeldItem(self, edx, device, flag);
        t_realCursor = real;
    }

    uint32_t __fastcall hookSavePortrait(void* self, void* edx, const char* path, int w, int h, float scale)
    {
        UiCanvas::Suspend physical;
        return g_origSavePortrait(self, edx, path, w, h, scale);
    }

    // Mouse events carry the cursor as the window procedure read it (UI coordinates). The UI gets them first;
    // the world mouse handler then picks with them, so it gets screen pixels for the duration of the call.
    uint32_t __fastcall hookWorldMouse(cEngine* engine, void* edx, cEvent* event, int flag)
    {
        cEventMouse* mouse = cEventMouse::of(event);
        if (!mouse)
        {
            return g_origWorldMouse(engine, edx, event, flag);
        }
        const int x = mouse->x, y = mouse->y;
        mouse->x = UiCanvas::toPhysicalX(x);
        mouse->y = UiCanvas::toPhysicalY(y);
        const uint32_t result = g_origWorldMouse(engine, edx, event, flag);
        if (Config::debug.uiTrace)
        {
            LOG("UI trace: world mouse {} at {},{} (screen {},{}) flag {} -> {}",
                *reinterpret_cast<uintptr_t*>(event) == Addr::cEventMouseDown_vtable ? "down" : "up", x, y, mouse->x,
                mouse->y, flag, result);
        }
        mouse->x = x;
        mouse->y = y;
        return result;
    }

    // [Debug] UiTrace: the tile a click lands on, as the world mouse handler looks it up (it walks only to a tile that
    // exists, has +8 == 0 and bit 0 of +0x1E).
    using TileAtFn = uint8_t*(__fastcall*)(void* map, void* edx, const int32_t* pos);
    TileAtFn g_origTileAt = nullptr;

    uint8_t* __fastcall tracedTileAt(void* map, void* edx, const int32_t* pos)
    {
        uint8_t* tile = g_origTileAt(map, edx, pos);
        LOG("UI trace: clicked tile region {} at {},{} level {}: {}", pos[0] & 0xFFFF, pos[1], pos[2], pos[3] & 0xFF,
            tile ? Fmt::format("+8 {} +1E {:02x}", *reinterpret_cast<int32_t*>(tile + 8), tile[0x1E]) : std::string("none"));
        return tile;
    }

    // [Debug] UiTrace: each mouse button event as the engine gets it, whether the UI claims its position, and what
    // the engine returns (the world mouse handler logs whether it got the event).
    uint32_t __fastcall hookReceiveEvent(cEngine* engine, void* edx, cEvent* event)
    {
        cEventMouse* mouse = cEventMouse::of(event);
        if (!mouse)
        {
            return g_origReceiveEvent(engine, edx, event);
        }
        cUI_Manager* manager = cUI_Manager::instance();
        const bool overUi = manager && manager->isCursorOverUi(mouse->x, mouse->y);
        LOG("UI trace: mouse {} at {},{} (screen {},{}), over the UI: {}",
            *reinterpret_cast<uintptr_t*>(event) == Addr::cEventMouseDown_vtable ? "down" : "up", mouse->x, mouse->y,
            UiCanvas::toPhysicalX(mouse->x), UiCanvas::toPhysicalY(mouse->y), overUi);
        const uint32_t result = g_origReceiveEvent(engine, edx, event);
        LOG("UI trace: mouse event -> {}", result);
        return result;
    }

    // The world cursor handler reads the mouse once (redirected to physical) and also asks the UI with it.
    // Mod UI (Sacred Gear) counts as UI there: the world picks nothing under it.
    bool __fastcall isCursorOverUiPhysical(cUI_Manager* manager, void* /*edx*/, int x, int y)
    {
        const int vx = UiCanvas::toVirtualX(x), vy = UiCanvas::toVirtualY(y);
        return modUiAt(vx, vy) || manager->isCursorOverUi(vx, vy);
    }

    int __fastcall physicalGetX(cMouse* mouse)
    {
        return UiCanvas::toPhysicalX(mouse->x);
    }

    int __fastcall physicalGetY(cMouse* mouse)
    {
        return UiCanvas::toPhysicalY(mouse->y);
    }

    // Stand-in cMouse for call sites that only read +4/+8 afterwards.
    void* __cdecl physicalMouseInstance()
    {
        thread_local int shadow[4];
        cMouse* mouse = cMouse::instance();
        shadow[1] = UiCanvas::toPhysicalX(mouse->x);
        shadow[2] = UiCanvas::toPhysicalY(mouse->y);
        return shadow;
    }

    // UI reads of the cursor: canvas coordinates moved into the calling thread's frame.
    // Under mod UI they see it far off the screen: no window hovers or shows a tooltip under a mod's panel.
    int frameX(int canvas) { return cursorHidden() ? kHiddenCursor : static_cast<int>(std::lround(canvas - t_frame.x)); }
    int frameY(int canvas) { return cursorHidden() ? kHiddenCursor : static_cast<int>(std::lround(canvas - t_frame.y)); }

    int __fastcall frameGetX(cMouse* mouse)
    {
        return frameX(mouse->x);
    }

    int __fastcall frameGetY(cMouse* mouse)
    {
        return frameY(mouse->y);
    }

    void* __cdecl frameMouseInstance()
    {
        cMouse* mouse = cMouse::instance();
        if (t_frame.x == 0.0f && t_frame.y == 0.0f && !cursorHidden())
        {
            return mouse;
        }
        thread_local int shadow[4];
        shadow[1] = frameX(mouse->x);
        shadow[2] = frameY(mouse->y);
        return shadow;
    }

    void __fastcall frameCursorPos(cMouse* mouse, void* /*edx*/, int* x, int* y)
    {
        Addr::cMouse_getCursorPos(mouse, x, y);
        if (mouse->cursorImage && cursorHidden())
        {
            *x = *y = kHiddenCursor;
        }
        else if (mouse->cursorImage)
        {
            *x = frameX(*x);
            *y = frameY(*y);
        }
    }
}

bool UiCanvas::enabled() { return g_enabled; }

bool UiCanvas::inGame()
{
    const cUI_Manager* manager = cUI_Manager::instance();
    return manager && (manager->flags & cUI_Manager::inGame);
}

void UiCanvas::updateCursorFilter()
{
    const cMouse* mouse = g_cursorFilter.load(std::memory_order_acquire) ? cMouse::instance() : nullptr;
    g_cursorHidden.store(mouse && modUiAt(mouse->x, mouse->y), std::memory_order_relaxed);
}

// Sacred Gear: `filter(x, y)` (canvas coordinates) tells whether mod UI lies there; nullptr: none. Returns 0 while
// the canvas is off (the UI's cursor reads are not SacredBild's then).
extern "C" int __cdecl SacredBild_UiSetCursorFilter(int(__cdecl* filter)(int x, int y))
{
    if (!g_enabled)
    {
        return 0;
    }
    g_cursorFilter.store(filter, std::memory_order_release);
    if (!filter)
    {
        g_cursorHidden = false;
    }
    return 1;
}

UiCanvas::Bounds UiCanvas::menuCanvas()
{
    return {g_menu.left, g_menu.top, g_menu.left + 1024.0f * g_menu.scale, g_menu.top + 768.0f * g_menu.scale};
}
float UiCanvas::left() { return layout().left; }
float UiCanvas::top() { return layout().top; }
float UiCanvas::right() { const Layout& l = layout(); return l.left + 1024.0f * l.scale; }
float UiCanvas::bottom() { const Layout& l = layout(); return l.top + 768.0f * l.scale; }

int UiCanvas::toVirtualX(int physical) { const Layout& l = layout(); return static_cast<int>(std::lround((physical - l.left) / l.scale)); }
int UiCanvas::toVirtualY(int physical) { const Layout& l = layout(); return static_cast<int>(std::lround((physical - l.top) / l.scale)); }
int UiCanvas::toPhysicalX(int virt) { const Layout& l = layout(); return static_cast<int>(std::lround(virt * l.scale + l.left)); }
int UiCanvas::toPhysicalY(int virt) { const Layout& l = layout(); return static_cast<int>(std::lround(virt * l.scale + l.top)); }

UiCanvas::Frame UiCanvas::placed(float x, float y)
{
    // Frames exist in game only. Whole pixels for the frame's origin, so the UI's texels keep their alignment (0.5
    // gives the canvas itself).
    const Layout& l = g_game;
    if (Sacred::reBorn())
    {
        // The windows' coordinates carry the offset of their anchor already, so against the centered frame (offset 0)
        // an edge anchor only has to make up for the scaling of that offset: physical = fraction * W * (1 - scale)
        // + x * scale. Their 1024x768 rects are not at the frame's origin, so they are not confined.
        const float w = static_cast<float>(Resolution::width()), h = static_cast<float>(Resolution::height());
        return {(std::clamp(x, 0.0f, 1.0f) - 0.5f) * w * (1.0f - l.scale) / l.scale,
                (std::clamp(y, 0.0f, 1.0f) - 0.5f) * h * (1.0f - l.scale) / l.scale, false};
    }
    const float originX = std::floor((Resolution::width() - 1024.0f * l.scale) * std::clamp(x, 0.0f, 1.0f));
    const float originY = std::floor((Resolution::height() - 768.0f * l.scale) * std::clamp(y, 0.0f, 1.0f));
    return {(originX - l.left) / l.scale, (originY - l.top) / l.scale, true};
}

UiCanvas::Frame UiCanvas::frame() { return t_frame; }

UiCanvas::Placement UiCanvas::placement()
{
    const float w = static_cast<float>(Resolution::width()), h = static_cast<float>(Resolution::height());
    const Layout& l = layout();
    Placement p;
    p.scale = l.scale;
    p.originX = l.left + t_frame.x * l.scale;
    p.originY = l.top + t_frame.y * l.scale;
    if (t_frame.confine)
    {
        p.clipLeft = std::max(p.originX, 0.0f);
        p.clipTop = std::max(p.originY, 0.0f);
        p.clipRight = std::min(p.originX + 1024.0f * l.scale, w);
        p.clipBottom = std::min(p.originY + 768.0f * l.scale, h);
    }
    else
    {
        p.clipLeft = 0.0f;
        p.clipTop = 0.0f;
        p.clipRight = w;
        p.clipBottom = h;
    }
    return p;
}

UiCanvas::Bounds UiCanvas::screenBounds()
{
    const Layout& l = layout();
    return {-l.left / l.scale - t_frame.x, -l.top / l.scale - t_frame.y,
            (Resolution::width() - l.left) / l.scale - t_frame.x, (Resolution::height() - l.top) / l.scale - t_frame.y};
}

bool UiCanvas::tracing() { return t_trace; }
bool UiCanvas::tracingPopups() { return Config::debug.uiTrace && GetTickCount64() < g_tracePopupsUntil.load(); }

void UiCanvas::trace(const std::string& line)
{
    LOG("UiTrace: {} | frame {:.1f},{:.1f}{} | callers{}", line, t_frame.x, t_frame.y, t_frame.confine ? "" : " unconfined",
        exeCallers());
}

UiCanvas::FrameScope::FrameScope(const Frame& frame) : m_previous(t_frame)
{
    if (frame == t_frame)
    {
        return;
    }
    t_frame = frame;
    notifyProxy();
}

UiCanvas::FrameScope::~FrameScope()
{
    if (m_previous == t_frame)
    {
        return;
    }
    t_frame = m_previous;
    notifyProxy();
}

void UiCanvas::enter(Mode mode)
{
    if (g_enabled && t_depth++ == 0)
    {
        t_outerFrame = t_frame;
        t_frame = {0.0f, 0.0f, mode == Mode::Canvas && !Sacred::reBorn()};     // ReBorn's windows reach past the block
        t_layout = &liveLayout();
        if (Config::debug.uiTrace && mode == Mode::Canvas)
        {
            const bool down = GetAsyncKeyState(VK_SCROLL) < 0;
            const bool autoTrace = g_autoTrace.load() > 0 && --g_autoTrace >= 0;
            if ((down && !g_traceKeyDown) || autoTrace)
            {
                t_trace = true;
                g_tracePopupsUntil = GetTickCount64() + 5000;
                LOG("UiTrace: frame begins (canvas {},{} scale {:.3f}; popups traced for 5 s)", t_layout->left,
                    t_layout->top, t_layout->scale);
            }
            g_traceKeyDown = down;
        }
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->beginUi();
        }
    }
}

void UiCanvas::leave()
{
    if (g_enabled && --t_depth == 0)
    {
        if (t_trace)
        {
            LOG("UiTrace: frame ends");
            t_trace = false;
        }
        t_frame = t_outerFrame;
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->endUi();
        }
        t_layout = nullptr;
    }
}

int UiCanvas::suspend()
{
    const int depth = t_depth;
    if (g_enabled && depth > 0)
    {
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->endUi();
        }
    }
    t_depth = 0;
    return depth;
}

void UiCanvas::resume(int depth)
{
    t_depth = depth;
    if (g_enabled && depth > 0)
    {
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->beginUi();
        }
    }
}

void UiCanvas::install()
{
    if (!Resolution::active())
    {
        return;
    }
    const float fit = std::min(Resolution::width() / 1024.0f, Resolution::height() / 768.0f);
    auto centered = [](float scale) {
        return Layout{scale, std::floor((Resolution::width() - 1024.0f * scale) / 2.0f),
                      std::floor((Resolution::height() - 768.0f * scale) / 2.0f)};
    };
    g_game = centered(Config::ui.scale > 0.0f ? std::min(Config::ui.scale, fit) : fit);
    g_menu = Config::ui.scaleMenus ? g_game : centered(fit);
    if (Sacred::reBorn())
    {
        // ReBorn's in-game windows sit in screen coordinates: each has the offset of the edge it is anchored to
        // ((W - 1024) * 0, 1/2 or 1 horizontally, likewise vertically) added to its 1024x768 position, the centered
        // ones included. The canvas frame (offset 0) is the centered placement: left + x * scale puts the block of
        // centered windows where the canvas would be. Windows anchored elsewhere get frames (placed). The menus
        // are drawn in 1024x768 at the top left, as in the other builds.
        const float cx = static_cast<float>((Resolution::width() - 1024) / 2), cy = static_cast<float>((Resolution::height() - 768) / 2);
        g_game = {g_game.scale, g_game.left - cx * g_game.scale, g_game.top - cy * g_game.scale};
        g_startMenu = {g_menu.scale, g_menu.left - cx * g_menu.scale, g_menu.top - cy * g_menu.scale};
    }
    g_enabled = true;
    LOG("UI canvas: in game scale {:.3f} at {},{}, menus scale {:.3f} at {},{}", g_game.scale, g_game.left, g_game.top,
        g_menu.scale, g_menu.left, g_menu.top);

    int redirected = 0;
    // World code that reads cMouse coordinates (getX / getY, or cMouse_instance() followed by reads of +4/+8) gets
    // physical-coordinate versions.
    for (uintptr_t site : Addr::worldGetXCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalGetX));
    for (uintptr_t site : Addr::worldGetYCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalGetY));
    for (uintptr_t site : Addr::worldMouseReads) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalMouseInstance));
    redirected += Patch::redirectCall(Addr::worldCursorUiTestCall, reinterpret_cast<void*>(&isCursorOverUiPhysical));
    LOG("UI canvas: {} world mouse reads redirected", redirected);
    {
        // The UI's own reads of the cursor follow the frame they run in (anchored windows, windows Sacred Gear moves)
        // and hide it under mod UI.
        redirected = 0;
        for (uintptr_t site : Addr::uiMouseReads) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&frameMouseInstance));
        for (uintptr_t site : Addr::uiGetXCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&frameGetX));
        for (uintptr_t site : Addr::uiGetYCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&frameGetY));
        for (uintptr_t site : Addr::uiCursorPosCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&frameCursorPos));
        LOG("UI canvas: {} UI mouse reads follow the frame", redirected);
    }

    Patch::hook(g_origGetClientCursorPos, Addr::getClientCursorPos, &hookGetClientCursorPos, "getClientCursorPos");
    Patch::hook(g_origRenderCursor, Addr::cMouse_renderCursor, &hookRenderCursor, "cMouse::renderCursor");
    Patch::hook(g_origSavePortrait, Addr::renderSavePortrait, &hookSavePortrait, "renderSavePortrait");
    Patch::hook(g_origHeldItem, Addr::cInventoryEntry_render, &hookHeldItem, "cInventoryEntry::render");
    Patch::hook(g_origWorldMouse, Addr::cEngine_worldMouse, &hookWorldMouse, "cEngine::worldMouse");
    if (Config::debug.uiTrace)
    {
        Patch::hook(g_origReceiveEvent, Addr::cEngine_receiveEvent, &hookReceiveEvent, "cEngine::receiveEvent");
        if (const uintptr_t site = Addr::worldMouseTileAtCall; site && Patch::verify(site, {0xE8}))
        {
            g_origTileAt = reinterpret_cast<TileAtFn>(site + 5 + *reinterpret_cast<const int32_t*>(site + 1));
            Patch::redirectCall(site, reinterpret_cast<void*>(&tracedTileAt));
        }
    }
}
