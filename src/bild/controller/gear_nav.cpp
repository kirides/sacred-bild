#include "gear_nav.h"
#include "bild_gear.h"
#include "ui/ui_canvas.h"
#include "gamepad.h"
#include "input_mode.h"
#include "prompts.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <mutex>

namespace
{
    // The layout Gear reports (its Ui::Command space); POD so that the DLLs need no shared headers.
    using NavStop = SacredBild_NavStop;
    constexpr int kModal = 1, kWindow = 2;
    constexpr int kMaxStops = 512;
    constexpr ULONGLONG kStaleMs = 600;

    std::mutex g_lock;
    std::vector<NavStop> g_stops;
    int g_flags = 0;
    ULONGLONG g_updated = 0;
    std::vector<float> g_native;      // Sacred Gear's stops that draw their own button prompt: left, top, width, height each (its layout units)

    std::atomic<int> g_nudge{0}, g_back{0}, g_menu{0}, g_scroll{0}, g_page{0}, g_select{0};
    using WheelFn = int(__cdecl*)(int delta);
    std::atomic<WheelFn> g_wheel{nullptr};

    bool fresh()
    {
        return g_updated && GetTickCount64() - g_updated < kStaleMs;
    }

    float px(float x) { return static_cast<float>(UiCanvas::toPhysicalX(static_cast<int>(std::lround(x)))); }
    float py(float y) { return static_cast<float>(UiCanvas::toPhysicalY(static_cast<int>(std::lround(y)))); }
}

bool GearNav::nativePrompt(float x, float y)
{
    std::lock_guard lock(g_lock);
    if (!fresh())
    {
        return false;
    }
    for (size_t i = 0; i + 3 < g_native.size(); i += 4)
    {
        const float l = px(g_native[i]), t = py(g_native[i + 1]), r = px(g_native[i] + g_native[i + 2]), b = py(g_native[i + 1] + g_native[i + 3]);
        if (x >= l && x < r && y >= t && y < b)
        {
            return true;
        }
    }
    return false;
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
        if ((s.kind & 3) == 2 && x >= s.left && x < s.right && y >= s.top && y < s.bottom)
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
void GearNav::postPage(int direction) { g_page += direction; }
void GearNav::postSelect() { ++g_select; }

// Sacred Gear, once per tick: the interactive rectangles of its widgets in the UI's layout coordinates (the order
// they were drawn in), and flags: 1 a modal mod screen (the stops are all there is), 2 a mod window is open.
extern "C" void __cdecl SacredBild_UiSetNavStops(const NavStop* stops, int count, int flags)
{
    std::lock_guard lock(g_lock);
    g_stops.assign(stops, stops + (stops && count > 0 ? std::min(count, kMaxStops) : 0));
    g_flags = flags;
    g_updated = GetTickCount64();
}

// Sacred Gear, optional: the stops (left, top, width, height each) whose control draws its own button prompt (an RmlUi element), so that no prompt
// of ours is drawn beside them. Sent every tick with the stops; a Gear without it (or no Gear) leaves the prompts as they are.
extern "C" void __cdecl SacredBild_UiSetNativePrompts(const float* rects, int count)
{
    std::lock_guard lock(g_lock);
    g_native.assign(rects, rects + (rects && count > 0 ? std::min(count, kMaxStops) * 4 : 0));
}

// Sacred Gear's automation (self tests without a pad): posts pad events as the controller would, and reads back
// what SacredBild holds (the stop count, -1 if stale; the flags in *flags).
extern "C" void __cdecl SacredBild_UiPostPad(int nudge, int back, int menu)
{
    g_nudge += nudge;
    g_back += back;
    g_menu += menu;
    // As a real pad press does: the prompts (body.pad) and the pad focus follow the pad from now on.
    if (nudge != 0 || back != 0 || menu != 0)
    {
        InputMode::controllerUsed();
    }
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

// Automation: the D-pad's up / down steps in a scroll area (+ down), as the controller posts them.
extern "C" void __cdecl SacredBild_UiPostScroll(int steps)
{
    g_scroll += steps;
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

// Sacred Gear: LB / RB while its screen or window is open, since the last call: the pages stepped (+ next, - previous; each press counts one).
extern "C" int __cdecl SacredBild_UiTakePage()
{
    return g_page.exchange(0);
}

// Sacred Gear: presses of the pad's Select / "-" button since the last call (the Mods button of the escape menu answers it).
extern "C" int __cdecl SacredBild_UiTakeSelect()
{
    return g_select.exchange(0);
}

// Automation: posts page steps as LB / RB would.
extern "C" void __cdecl SacredBild_UiPostPage(int pages)
{
    if (pages == 100)
    {
        ++g_select;     // the automation's Select press
    }
    else
    {
        g_page += pages;
    }
    if (pages != 0)
    {
        InputMode::controllerUsed();
    }
}

// Sacred Gear's controller prompts: the style of the pad in use (0 Xbox, 1 PlayStation, 2 Switch) and whether the
// controller was used last (1) rather than the keyboard and mouse.
extern "C" int __cdecl SacredBild_PadInfo(int* style)
{
    *style = static_cast<int>(Gamepad::state().style);
    return InputMode::controller() ? 1 : 0;
}

// The 64x64 icon of a button (a Gamepad::Button bit) of a style as 4096 premultiplied ARGB pixels; 1 if there is one.
extern "C" int __cdecl SacredBild_PadGlyph(int style, unsigned button, unsigned* pixels)
{
    if (style < 0 || style > 2)
    {
        return 0;
    }
    return Prompts::glyph(static_cast<Gamepad::Style>(style), button, pixels) ? 1 : 0;
}
