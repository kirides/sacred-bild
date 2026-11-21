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
        int kind;       // 0 button, 1 slider, 2 scroll area (+ 4: it is scrolled to its top, + 8: to its bottom)
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
    // Whether (x, y) (screen pixels) is on a stop whose control draws its own button prompt: none is drawn beside it.
    bool nativePrompt(float x, float y);
    // The scroll area stop (x, y) lies in, false if none (D-pad up / down there scrolls it at the ends).
    bool scrollAreaAt(float x, float y, Stop& out);

    // The mouse wheel (WM_MOUSEWHEEL, the player's or the controller's): Gear takes it while the cursor is over a mod's
    // widget; true if it did (the game then does not get it).
    bool wheel(int delta);

    // Pad events for Gear.
    void postNudge(int steps);
    void postScroll(int steps);     // D-pad up / down in a scroll area: lines, + down
    void postBack();
    void postMenu();
    void postSelect();              // the pad's Select / "-" button
    void postPage(int direction);   // LB (-1) / RB (+1) while a mod screen or window is open: its previous / next page
}
