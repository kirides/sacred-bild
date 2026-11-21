#pragma once
#include <vector>

// The controller side of the mod UI, through SacredBild (its exports; without SacredBild or a controller, nothing
// happens): the interactive rectangles of the widgets, reported every tick so the D-pad can jump between them, and
// the pad events Gear takes back (slider nudges, B, the mod menu button).
namespace PadNav
{
    struct Stop
    {
        float x, y;                 // where the cursor goes
        float left, top, width, height;
        int kind;                   // 0 button, 1 slider
    };
    enum Flags { Modal = 1, Window = 2 };

    // True if SacredBild takes them.
    bool setStops(const std::vector<Stop>& stops, int flags);
    // The events since the last call.
    struct Events
    {
        int nudge = 0, back = 0, menu = 0;
    };
    Events take();
    // Automation: pad events as the controller would post them; SacredBild's stop count (-1 stale, -2 no SacredBild).
    void post(int nudge, int back, int menu);
    int takeSelect();               // presses of Select / "-" since the last call
    int takePage();                 // LB / RB since the last call: + next, - previous
    void postPage(int pages);       // automation
    int navState(int& flags);
    // Whether the controller was used last (not the keyboard and mouse) and the pad's style (0 Xbox, 1 PlayStation,
    // 2 Switch); false, Xbox without SacredBild.
    bool padInfo(int& style);
    // The 64x64 icon of a button (Gamepad::Button's bit: A = 1, B = 2 ...) as premultiplied ARGB pixels, in the pad's style.
    bool glyph(unsigned button, std::vector<unsigned>& pixels);
}
