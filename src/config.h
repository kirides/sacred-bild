#pragma once
#include <string>

// Settings from SacredBild.ini next to the game exe.
struct Config
{
    // Chain-loaded ddraw implementation; empty or missing falls back to the system ddraw.dll.
    std::wstring ddrawChain = L"SacredBild\\DDrawCompat.dll";

    // Render resolution; 0 = desktop size. 1024x768 runs the game unpatched.
    int width = 0;
    int height = 0;
    bool borderless = true;       // main window without frame, client area = back buffer

    // UI canvas: the 1024x768 UI drawn centered. 0 = scale to fit the screen height.
    float uiScale = 0.0f;
    bool uiLinearFilter = true;   // bilinear filtering for the scaled UI instead of the game's point sampling

    // Texture memory the game may keep loaded, in MB; 0 = max(game's own value, 256). A zoomed-out view at a
    // high resolution shows far more different ground textures than the original 1024x768.
    int textureBudgetMB = 0;

    // Diagnostics
    bool d3dStats = true;         // per-second D3D7 call counts in the log
    bool profiler = false;        // sample the render thread, dump hot spots on exit
    int profilerIntervalUs = 500;
};

extern Config g_config;

namespace ConfigFile
{
    void load(const std::wstring& gameDir);
}
