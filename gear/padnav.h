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
    int navState(int& flags);
}
