#include "game/gear_nav.h"
#include "game/ui_canvas.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <mutex>

namespace
{
    // The layout Gear reports (its Ui::Command space); POD so that the DLLs need no shared headers.
    struct NavStop
    {
        float x, y;
        float left, top, width, height;
        int32_t kind;
    };
    constexpr int kModal = 1, kWindow = 2;
    constexpr int kMaxStops = 512;
    constexpr ULONGLONG kStaleMs = 600;

    std::mutex g_lock;
    std::vector<NavStop> g_stops;
    int g_flags = 0;
    ULONGLONG g_updated = 0;

    std::atomic<int> g_nudge{0}, g_back{0}, g_menu{0}, g_scroll{0};
    using WheelFn = int(__cdecl*)(int delta);
    std::atomic<WheelFn> g_wheel{nullptr};

    bool fresh()
    {
        return g_updated && GetTickCount64() - g_updated < kStaleMs;
    }

    float px(float x) { return static_cast<float>(UiCanvas::toPhysicalX(static_cast<int>(std::lround(x)))); }
    float py(float y) { return static_cast<float>(UiCanvas::toPhysicalY(static_cast<int>(std::lround(y)))); }
}

std::vector<GearNav::Stop> GearNav::stops()
{
    std::vector<NavStop> copy;
    {
        std::lock_guard lock(g_lock);
        if (!fresh())
        {
            return {};
        }
        copy = g_stops;
    }
    std::vector<Stop> out;
    out.reserve(copy.size());
    for (const NavStop& s : copy)
    {
        out.push_back({px(s.x), py(s.y), px(s.left), py(s.top), px(s.left + s.width), py(s.top + s.height), s.kind});
    }
    return out;
}

bool GearNav::modal()
{
    std::lock_guard lock(g_lock);
    return fresh() && (g_flags & kModal) && !g_stops.empty();
}

bool GearNav::windowOpen()
{
    std::lock_guard lock(g_lock);
    return fresh() && (g_flags & (kModal | kWindow));
}

bool GearNav::home(float& x, float& y)
{
    std::lock_guard lock(g_lock);
    if (!fresh() || g_stops.empty())
    {
        return false;
    }
    x = px(g_stops.front().x);
    y = py(g_stops.front().y);
    return true;
}

bool GearNav::onSlider(float x, float y)
{
    for (const Stop& s : stops())
    {
        if (s.kind == 1 && x >= s.left && x < s.right && y >= s.top && y < s.bottom)
        {
            return true;
        }
    }
    return false;
}

bool GearNav::scrollAreaAt(float x, float y, Stop& out)
{
    for (const Stop& s : stops())
    {
        if (s.kind == 2 && x >= s.left && x < s.right && y >= s.top && y < s.bottom)
        {
            out = s;
            return true;
        }
    }
    return false;
}

bool GearNav::wheel(int delta)
{
    const WheelFn fn = g_wheel.load(std::memory_order_acquire);
    return fn && fn(delta) != 0;
}

void GearNav::postScroll(int steps) { g_scroll += steps; }
void GearNav::postNudge(int steps) { g_nudge += steps; }
void GearNav::postBack() { ++g_back; }
void GearNav::postMenu() { ++g_menu; }

// Sacred Gear, once per tick: the interactive rectangles of its widgets in the UI's layout coordinates (the order
// they were drawn in), and flags: 1 a modal mod screen (the stops are all there is), 2 a mod window is open.
extern "C" void __cdecl SacredBild_UiSetNavStops(const NavStop* stops, int count, int flags)
{
    std::lock_guard lock(g_lock);
    g_stops.assign(stops, stops + (stops && count > 0 ? std::min(count, kMaxStops) : 0));
    g_flags = flags;
    g_updated = GetTickCount64();
}

// Sacred Gear's automation (self tests without a pad): posts pad events as the controller would, and reads back
// what SacredBild holds (the stop count, -1 if stale; the flags in *flags).
extern "C" void __cdecl SacredBild_UiPostPad(int nudge, int back, int menu)
{
    g_nudge += nudge;
    g_back += back;
    g_menu += menu;
}

extern "C" int __cdecl SacredBild_UiNavState(int* flags)
{
    std::lock_guard lock(g_lock);
    *flags = g_flags;
    return fresh() ? static_cast<int>(g_stops.size()) : -1;
}

// Sacred Gear: its mouse wheel handler (delta as WM_MOUSEWHEEL has it; returns 1 if the wheel went to mod UI, which
// the game then does not get), and the D-pad's scroll steps in a scroll area since the last call (+ down).
extern "C" void __cdecl SacredBild_UiSetWheelHandler(int(__cdecl* fn)(int delta))
{
    g_wheel.store(fn, std::memory_order_release);
}

extern "C" int __cdecl SacredBild_UiTakeScroll()
{
    return g_scroll.exchange(0);
}

// Sacred Gear, once per tick: the pad's events since the last call (each count is then reset).
extern "C" int __cdecl SacredBild_UiTakePad(int* nudge, int* back, int* menu)
{
    *nudge = g_nudge.exchange(0);
    *back = g_back.exchange(0);
    *menu = g_menu.exchange(0);
    return 1;
}
