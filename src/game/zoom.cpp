#include "game/zoom.h"
#include "config/camera.h"
#include "config/debug.h"
#include "game/focus.h"
#include "game/ui_state.h"
#include "log.h"
#include "patch.h"
#include "sacred/engine.h"
#include "sacred/mouse.h"
#include "sacred/world.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
    using namespace Sacred;
    using Config::Camera;

    decltype(Addr::cEngine_handleZoomInput)::Ptr g_orig = nullptr;     // the game's own function, not detoured

    constexpr float kHoldRate = 1.1f;       // a held key changes the zoom by this factor's exponent per second (2 -> 0.5 in ~1.3 s)
    constexpr float kNotch = 0.86f;         // a wheel notch scales the zoom by this
    constexpr float kEase = 16.0f;          // the shown zoom closes on the target by 1 - exp(-kEase * dt) per frame (~60 ms)
    constexpr float kRepeat = 0.35f;        // Steps: a held key steps again after this many seconds

    std::atomic<int> g_wheelDelta{0};       // from the window thread, in WM_MOUSEWHEEL units (120 = one notch)

    // The game thread's.
    bool g_known = false;
    float g_target = 1.0f;
    float g_shown = 1.0f;       // what we last gave the view
    bool g_keyIn = false, g_keyOut = false;
    float g_keyTime = 0.0f;
    bool g_startApplied = false;

    // The first step beyond `zoom` in the direction (-1 = closer, +1 = farther), within the limits; the limit itself
    // when no step is left.
    float nextStep(float zoom, int direction)
    {
        const Camera& c = Config::camera;
        float best = direction < 0 ? c.zoomMin : c.zoomMax;
        for (float s : c.steps)
        {
            if (direction < 0 && s < zoom - 0.005f)
            {
                best = std::max(best, s);   // the steps are sorted, the limit is the floor
            }
            else if (direction > 0 && s > zoom + 0.005f)
            {
                best = std::min(best, s);
            }
        }
        return best;
    }

    bool keyDown(int vk)
    {
        return Focus::keyDown(vk);
    }

    void __fastcall hookHandleZoom(cEngine* engine, void* edx, float dt)
    {
        const Camera& c = Config::camera;
        if (c.zoom == Camera::Zoom::Original)
        {
            g_orig(engine, edx, dt);
            return;
        }
        cWorldView* view = engine->worldView();
        if (!view)
        {
            return;
        }
        dt = std::clamp(dt, 0.0f, 0.1f);
        const float speed = c.zoomSpeed / 100.0f;
        // Something else set the zoom (a game start, a script): follow it.
        if (!g_known || std::abs(view->zoom - g_shown) > 1e-4f)
        {
            g_target = g_shown = view->zoom;
            g_known = true;
            g_startApplied = false;
        }
        if (!g_startApplied && c.startZoom > 0.0f)
        {
            g_target = c.startZoom;
        }
        g_startApplied = true;

        const int wheelDelta = g_wheelDelta.exchange(0);
        const float notches = static_cast<float>(wheelDelta) / 120.0f;
        const bool blocked = *Addr::getPauseState() != 0 || *Addr::g_textInputActive != 0 || keyDown(VK_SHIFT);
        const bool in = !blocked && keyDown(VK_ADD);    // numpad +: closer
        const bool out = !blocked && keyDown(VK_SUBTRACT);

        if (c.zoom == Camera::Zoom::Smooth)
        {
            const float hold = (in ? -1.0f : 0.0f) + (out ? 1.0f : 0.0f);
            g_target *= std::exp(hold * kHoldRate * speed * dt);
            g_target *= std::pow(kNotch, notches * speed);
        }
        else
        {
            g_keyTime += dt;
            const bool press = (in && !g_keyIn) || (out && !g_keyOut);
            const bool repeat = (in || out) && g_keyTime >= kRepeat;
            if (press || repeat)
            {
                g_keyTime = 0.0f;
                g_target = nextStep(g_target, in ? -1 : 1);
            }
            if (notches != 0.0f)
            {
                const int count = std::max(1, static_cast<int>(std::lround(std::abs(notches))));
                for (int i = 0; i < count; ++i)
                {
                    g_target = nextStep(g_target, notches > 0 ? -1 : 1);
                }
            }
        }
        if (Config::debug.uiTrace && (in || out || wheelDelta))
        {
            LOG("Zoom: in {} out {} wheel {} blocked {} (pause {} text {}), target {:.3f} shown {:.3f}", in, out, wheelDelta, blocked,
                static_cast<int>(*Addr::getPauseState()), static_cast<int>(*Addr::g_textInputActive), g_target, view->zoom);
        }
        g_keyIn = in;
        g_keyOut = out;
        g_target = std::clamp(g_target, c.zoomMin, c.zoomMax);

        const float diff = g_target - g_shown;
        float next = g_shown + diff * (1.0f - std::exp(-kEase * dt));
        if (std::abs(g_target - next) < 0.0005f)
        {
            next = g_target;
        }
        if (next != view->zoom)
        {
            Addr::cWorldView_setZoom(view, next);
            g_shown = view->zoom;
        }
    }
}

void Zoom::install()
{
    if (Config::camera.zoom == Camera::Zoom::Original || !Addr::cEngine_handleZoomInput || !Addr::zoomInputCall || !Addr::cWorldView_setZoom ||
        !Addr::getPauseState || !Addr::g_textInputActive)
    {
        return;
    }
    g_orig = Addr::cEngine_handleZoomInput.ptr();
    const bool ok = Patch::redirectCall(Addr::zoomInputCall, reinterpret_cast<const void*>(&hookHandleZoom));
    LOG("Zoom: {} mode, {:.2f} .. {:.2f}, call site {}", Config::camera.zoom == Camera::Zoom::Smooth ? "smooth" : "steps", Config::camera.zoomMin, Config::camera.zoomMax, ok ? "retargeted" : "not found");
}

bool Zoom::wheel(int delta)
{
    if (!g_orig || !Config::camera.zoomWheel || delta == 0 || UiState::mode() != UiState::Mode::Game ||
        UiState::exclusiveWindow() || UiState::typing() || UiState::inputBlocked())
    {
        return false;
    }
    cUI_Manager* manager = UiState::manager();
    cMouse* mouse = cMouse::instance();
    if (!manager || !mouse || manager->isCursorOverUi(mouse->x, mouse->y))
    {
        return false;
    }
    g_wheelDelta.fetch_add(delta);
    return true;
}
