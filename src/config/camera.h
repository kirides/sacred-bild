#pragma once
#include <vector>

// [Camera] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Camera
    {
        // Original = the game's own zoom (numpad +/-, three steps 0.5 / 1 / 2); Steps = the list below, one step per
        // key press or wheel notch; Smooth = any zoom between the limits, easing into the target.
        enum class Zoom
        {
            Original,
            Steps,
            Smooth,
        };
        Zoom zoom = Zoom::Smooth;
        // The view's size as a factor of the screen: 0.5 shows half the width and height (everything twice as large).
        float zoomMin = 0.5f;       // closest
        float zoomMax = 2.0f;       // farthest (the game's own limit; the ground's far plane ends near there)
        std::vector<float> steps{0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        int zoomSpeed = 100;        // percent: how fast a held zoom key or a wheel notch moves the zoom
        float startZoom = 0.0f;     // automation (-gear-zoom=N): the zoom a game starts at, 0 = the game's own
        bool zoomWheel = true;      // the mouse wheel (and the controller's right stick) zooms while the cursor is over the world
    };
    inline Camera camera;
}
