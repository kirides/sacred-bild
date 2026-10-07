#pragma once
#include <windows.h>

// What surrounds the game's 1024x768 screens (menus, loading screen, full-screen windows in game) when they are drawn
// into the centered UI canvas, instead of black bars: the screen itself, blurred and darkened, scaled to cover the
// whole render target (menus, loading screen), or the game's world beside the window, darkened. Drawn on the Direct3D
// 9 device's current render target with the device state saved and restored around it; does nothing without the
// Direct3D 9 backend.
namespace DDraw9::Backdrop
{
    // Takes `canvas` (render target pixels) as a small blurred copy for fill(). Outside a scene.
    void capture(const RECT& canvas);
    // Draws the last capture outside `canvas`; false if there is none (or no Direct3D 9 device).
    bool fill(const RECT& canvas);
    // Darkens what is outside `canvas`; false without a Direct3D 9 device.
    bool dim(const RECT& canvas);
    // The next fill() has no capture to draw (another kind of screen comes up).
    void forget();
}
