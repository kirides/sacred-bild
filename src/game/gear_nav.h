#pragma once
#include <vector>

// Controller navigation for Sacred Gear's mod UI. Gear (a separate DLL) reports the interactive rectangles of its
// widgets each tick (SacredBild_UiSetNavStops); they join the D-pad's stops next to the game's own controls, so the
// cursor jumps to a mod's buttons the way it does to the game's. Gear gets back what only the pad can say: a
// slider's nudge (D-pad left / right on it), B (back), and the mod menu button (SacredBild_UiTakePad).
namespace GearNav
{
    struct Stop
    {
        float x, y;     // where the cursor goes, screen pixels
        float left, top, right, bottom;
        int kind;       // 0 button, 1 slider
    };

    // The reported stops, in screen pixels; empty when Gear has not reported within the last moments.
    std::vector<Stop> stops();
    // A mod screen is up that takes the pad alone: only its stops are D-pad targets, B is its back.
    bool modal();
    // A mod window (a closable panel) is open: the pad drives the cursor, B closes the window.
    bool windowOpen();
    // The stop the cursor starts on when a modal screen opens; false if there is none.
    bool home(float& x, float& y);
    // Whether (x, y) lies on a slider stop (D-pad left / right then nudge it instead of jumping).
    bool onSlider(float x, float y);

    // Pad events for Gear.
    void postNudge(int steps);
    void postBack();
    void postMenu();
}
