#pragma once

// The world view's zoom ([Camera] in SacredBild.ini). The game zooms with the numpad +/- keys, and a zoom between its
// three steps (0.5, 1, 2) glides on to the next one (cEngine_handleZoomInput, each frame). Here that function is
// replaced: Steps takes a list of steps, Smooth any zoom between the limits; both ease into the target a little, so a
// held key, a wheel notch and the controller (the zoom buttons are the numpad keys, the right stick is the wheel)
// all feel the same. The mouse wheel zooms while the cursor is over the world, not over a window or a mod's widget.
namespace Zoom
{
    // Queues the hook in the caller's Patch transaction.
    void install();

    // A mouse wheel event (WM_MOUSEWHEEL delta, positive = up) from the window's thread; true if it zoomed and the
    // game should not see it.
    bool wheel(int delta);
}
