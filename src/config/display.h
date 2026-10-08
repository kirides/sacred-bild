#pragma once

// [Display] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Display
    {
        // Render resolution; 0 = desktop size. 1024x768 runs the game unpatched.
        int width = 0;
        int height = 0;
        // Main window frame ([Display] Borderless): Auto = a frame (caption, system menu) when the window is smaller
        // than the screen, Never (Borderless=1) = the game's frameless popup, Always (Borderless=0). The client area is
        // the render resolution either way.
        enum class Frame
        {
            Auto,
            Never,
            Always,
        };
        Frame frame = Frame::Auto;
        bool clipCursor = true;       // mouse confined to the game window while it is in the foreground
        int fpsLimit = 60;            // the game's own in-game frame limit (it uses 60); 0 = off
        int fpsLimitInactive = 20;    // frame limit (menus and game) while none of the game's windows is in the foreground; 0 = off
        // Direct3D 9 backend: wait for the display's refresh when presenting, and how many frames the CPU may
        // queue ahead of the GPU (1 = lowest input latency).
        bool vsync = true;
        int maxFrameLatency = 1;
    };
    inline Display display;
}
