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
    int fpsLimit = 60;            // the game's own in-game frame limit (it uses 60); 0 = off

    // UI canvas: the 1024x768 UI drawn centered. 0 = scale to fit the screen height.
    float uiScale = 0.0f;
    bool uiLinearFilter = true;   // bilinear filtering for the scaled UI instead of the game's point sampling

    // Texture memory the game may keep loaded, in MB; 0 = max(game's own value, 256). A zoomed-out view at a
    // high resolution shows far more different ground textures than the original 1024x768.
    int textureBudgetMB = 0;

    // World view draws: merge consecutive draws that end up with the same device state into one call.
    bool batch = true;
    // Batched draws skip Direct3D 7's software clipping of pretransformed vertices; the GPU clips them.
    bool batchNoClip = true;
    // Batched draws go through vertex buffers instead of user memory (saves copies in the runtime and driver).
    bool batchVertexBuffer = true;
    // 3D models (characters and their shadows) go through the batcher too: vertex buffers, merged where possible.
    bool batchModels = true;
    // GrannyAdvanceTime runs on a worker thread, overlapping the start of the frame.
    bool asyncAnimation = true;
    // Hash index in front of the map data's record caches (std::map lookups per tile and object).
    bool recordIndex = true;
    // Copy small textures into shared pages so draws with different textures can be merged as well.
    bool atlas = true;
    int atlasPageSize = 8192;       // texels per side, clamped to the device limit
    int atlasPages = 2;             // at most this many pages per texture format
    int atlasMaxTextureSize = 512;  // larger textures are used directly

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
