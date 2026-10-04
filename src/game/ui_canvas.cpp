#include "game/ui_canvas.h"
#include "game/device_proxy.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Sacred;

    bool g_enabled = false;
    float g_scale = 1.0f;
    float g_left = 0.0f;
    float g_top = 0.0f;
    thread_local int t_depth = 0;
    thread_local UiCanvas::Frame t_frame;
    thread_local UiCanvas::Frame t_outerFrame;     // the frame before the outermost UI scope

    using GetClientCursorPosFn = void(__cdecl*)(HWND, POINT*);
    using RenderCursorFn = void(__fastcall*)(void* self, void* edx, void* device, int flag);
    using MouseInstanceFn = void*(__cdecl*)();
    using IsCursorOverUiFn = bool(__fastcall*)(void* uiManager, void* edx, int x, int y);
    using WorldMouseFn = uint32_t(__fastcall*)(void* engine, void* edx, void* event, int flag);
    WorldMouseFn g_origWorldMouse = nullptr;
    using SavePortraitFn = uint32_t(__fastcall*)(void* self, void* edx, const char* path, int w, int h, float scale);
    using CursorPosFn = void(__fastcall*)(void* mouse, void* edx, int* x, int* y);

    GetClientCursorPosFn g_origGetClientCursorPos = nullptr;
    RenderCursorFn g_origRenderCursor = nullptr;
    MouseInstanceFn g_mouseInstance = nullptr;
    IsCursorOverUiFn g_isCursorOverUi = nullptr;
    SavePortraitFn g_origSavePortrait = nullptr;
    CursorPosFn g_cursorPos = nullptr;

    int mouseField(void* mouse, uintptr_t offset)
    {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mouse) + offset);
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

    void __cdecl hookGetClientCursorPos(HWND hwnd, POINT* pt)
    {
        g_origGetClientCursorPos(hwnd, pt);
        if (pt)
        {
            pt->x = UiCanvas::toVirtualX(pt->x);
            pt->y = UiCanvas::toVirtualY(pt->y);
        }
    }

    void __fastcall hookRenderCursor(void* self, void* edx, void* device, int flag)
    {
        UiCanvas::Scope ui{UiCanvas::Mode::Overlay};
        UiCanvas::FrameScope canvas{{0.0f, 0.0f, false}};
        g_origRenderCursor(self, edx, device, flag);
    }

    uint32_t __fastcall hookSavePortrait(void* self, void* edx, const char* path, int w, int h, float scale)
    {
        UiCanvas::Suspend physical;
        return g_origSavePortrait(self, edx, path, w, h, scale);
    }

    // Mouse events carry the cursor as the window procedure read it (UI coordinates). The UI gets them first;
    // the world mouse handler then picks with them, so it gets screen pixels for the duration of the call.
    uint32_t __fastcall hookWorldMouse(void* engine, void* edx, void* event, int flag)
    {
        const uintptr_t vtable = event ? *static_cast<uintptr_t*>(event) : 0;
        if (vtable != Addr::cEventMouseDown_vtable && vtable != Addr::cEventMouseUp_vtable)
        {
            return g_origWorldMouse(engine, edx, event, flag);
        }
        auto* coords = reinterpret_cast<int*>(static_cast<uint8_t*>(event) + MouseEvent::x);
        const int x = coords[0], y = coords[1];
        coords[0] = UiCanvas::toPhysicalX(x);
        coords[1] = UiCanvas::toPhysicalY(y);
        const uint32_t result = g_origWorldMouse(engine, edx, event, flag);
        coords[0] = x;
        coords[1] = y;
        return result;
    }

    // The world cursor handler reads the mouse once (redirected to physical) and also asks the UI with it.
    bool __fastcall isCursorOverUiPhysical(void* uiManager, void* edx, int x, int y)
    {
        return g_isCursorOverUi(uiManager, edx, UiCanvas::toVirtualX(x), UiCanvas::toVirtualY(y));
    }

    int __fastcall physicalGetX(void* mouse)
    {
        return UiCanvas::toPhysicalX(mouseField(mouse, Mouse::x));
    }

    int __fastcall physicalGetY(void* mouse)
    {
        return UiCanvas::toPhysicalY(mouseField(mouse, Mouse::y));
    }

    // Stand-in cMouse for call sites that only read +4/+8 afterwards.
    void* __cdecl physicalMouseInstance()
    {
        thread_local int shadow[4];
        void* mouse = g_mouseInstance();
        shadow[1] = UiCanvas::toPhysicalX(mouseField(mouse, Mouse::x));
        shadow[2] = UiCanvas::toPhysicalY(mouseField(mouse, Mouse::y));
        return shadow;
    }

    // UI reads of the cursor: canvas coordinates moved into the calling thread's frame.
    int frameX(int canvas) { return static_cast<int>(std::lround(canvas - t_frame.x)); }
    int frameY(int canvas) { return static_cast<int>(std::lround(canvas - t_frame.y)); }

    int __fastcall frameGetX(void* mouse)
    {
        return frameX(mouseField(mouse, Mouse::x));
    }

    int __fastcall frameGetY(void* mouse)
    {
        return frameY(mouseField(mouse, Mouse::y));
    }

    void* __cdecl frameMouseInstance()
    {
        void* mouse = g_mouseInstance();
        if (t_frame.x == 0.0f && t_frame.y == 0.0f)
        {
            return mouse;
        }
        thread_local int shadow[4];
        shadow[1] = frameX(mouseField(mouse, Mouse::x));
        shadow[2] = frameY(mouseField(mouse, Mouse::y));
        return shadow;
    }

    void __fastcall frameCursorPos(void* mouse, void* edx, int* x, int* y)
    {
        g_cursorPos(mouse, edx, x, y);
        if (*reinterpret_cast<void**>(static_cast<uint8_t*>(mouse) + Mouse::cursorImage))
        {
            *x = frameX(*x);
            *y = frameY(*y);
        }
    }
}

bool UiCanvas::enabled() { return g_enabled; }
float UiCanvas::scale() { return g_scale; }
float UiCanvas::left() { return g_left; }
float UiCanvas::top() { return g_top; }
float UiCanvas::right() { return g_left + 1024.0f * g_scale; }
float UiCanvas::bottom() { return g_top + 768.0f * g_scale; }

int UiCanvas::toVirtualX(int physical) { return static_cast<int>(std::lround((physical - g_left) / g_scale)); }
int UiCanvas::toVirtualY(int physical) { return static_cast<int>(std::lround((physical - g_top) / g_scale)); }
int UiCanvas::toPhysicalX(int virt) { return static_cast<int>(std::lround(virt * g_scale + g_left)); }
int UiCanvas::toPhysicalY(int virt) { return static_cast<int>(std::lround(virt * g_scale + g_top)); }

UiCanvas::Frame UiCanvas::anchored(int horizontal, int vertical)
{
    // Whole pixels for the frame's origin, so the UI's texels stay where they are in the canvas.
    const float originX[3] = {0.0f, g_left, std::floor(Resolution::width() - 1024.0f * g_scale)};
    const float originY[3] = {0.0f, g_top, std::floor(Resolution::height() - 768.0f * g_scale)};
    return {(originX[std::clamp(horizontal, 0, 2)] - g_left) / g_scale,
            (originY[std::clamp(vertical, 0, 2)] - g_top) / g_scale, true};
}

UiCanvas::Frame UiCanvas::frame() { return t_frame; }

UiCanvas::Placement UiCanvas::placement()
{
    const float w = static_cast<float>(Resolution::width()), h = static_cast<float>(Resolution::height());
    Placement p;
    p.scale = g_scale;
    p.originX = g_left + t_frame.x * g_scale;
    p.originY = g_top + t_frame.y * g_scale;
    if (t_frame.confine)
    {
        p.clipLeft = std::max(p.originX, 0.0f);
        p.clipTop = std::max(p.originY, 0.0f);
        p.clipRight = std::min(p.originX + 1024.0f * g_scale, w);
        p.clipBottom = std::min(p.originY + 768.0f * g_scale, h);
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
        t_frame = {0.0f, 0.0f, mode == Mode::Canvas};
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
        t_frame = t_outerFrame;
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->endUi();
        }
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

bool UiCanvas::active()
{
    return g_enabled && t_depth > 0;
}

void UiCanvas::install()
{
    if (!Resolution::active())
    {
        return;
    }
    const float fit = std::min(Resolution::width() / 1024.0f, Resolution::height() / 768.0f);
    g_scale = g_config.uiScale > 0.0f ? std::min(g_config.uiScale, fit) : fit;
    g_left = std::floor((Resolution::width() - 1024.0f * g_scale) / 2.0f);
    g_top = std::floor((Resolution::height() - 768.0f * g_scale) / 2.0f);
    g_enabled = true;
    LOG("UI canvas: scale {:.3f} at {},{} ({}x{})", g_scale, g_left, g_top, 1024.0f * g_scale, 768.0f * g_scale);

    g_mouseInstance = reinterpret_cast<MouseInstanceFn>(Addr::cMouse_instance);
    g_isCursorOverUi = reinterpret_cast<IsCursorOverUiFn>(Addr::cUI_Manager_isCursorOverUi);
    int redirected = 0;
    // World code that reads cMouse coordinates (getX / getY, or cMouse_instance() followed by reads of +4/+8) gets
    // physical-coordinate versions.
    for (uintptr_t site : Addr::worldGetXCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalGetX));
    for (uintptr_t site : Addr::worldGetYCalls) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalGetY));
    for (uintptr_t site : Addr::worldMouseReads) redirected += Patch::redirectCall(site, reinterpret_cast<void*>(&physicalMouseInstance));
    redirected += Patch::redirectCall(Addr::worldCursorUiTestCall, reinterpret_cast<void*>(&isCursorOverUiPhysical));
    LOG("UI canvas: {} world mouse reads redirected", redirected);
    if (g_config.uiAnchor)
    {
        // The UI's own reads of the cursor follow the frame they run in.
        g_cursorPos = reinterpret_cast<CursorPosFn>(Addr::cMouse_getCursorPos);
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
    Patch::hook(g_origWorldMouse, Addr::cEngine_worldMouse, &hookWorldMouse, "cEngine::worldMouse");
}
