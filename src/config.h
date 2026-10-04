#pragma once
#include <string>

// Settings from SacredBild.ini next to the game exe.
struct Config
{
    // DirectDraw / Direct3D 7 on SacredBild's own Direct3D 9Ex backend (Backend=d3d9), or on the chain-loaded ddraw
    // (Backend=chain): `ddrawChain`, or the system ddraw.dll if that is empty or missing.
    bool ddrawD3D9 = true;
    std::wstring ddrawChain = L"SacredBild\\DDrawCompat.dll";
    // Movies through Media Foundation instead of the game's DirectShow/DirectDraw path. Backend=d3d9 always does
    // (DirectShow can't decode into its surfaces); with Backend=chain this decides.
    bool mediaFoundation = true;

    // Render resolution; 0 = desktop size. 1024x768 runs the game unpatched.
    int width = 0;
    int height = 0;
    bool borderless = true;       // main window without frame, client area = back buffer
    int fpsLimit = 60;            // the game's own in-game frame limit (it uses 60); 0 = off
    // Direct3D 9 backend: wait for the display's refresh when presenting, and how many frames the CPU may
    // queue ahead of the GPU (1 = lowest input latency).
    bool vsync = true;
    int maxFrameLatency = 1;

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

    // LAN games over VPNs. Hosting: the gameserver Sacred starts gets SacredBild as well; it announces the game on
    // every network adapter with that adapter's address and to SacredBild players that subscribe at UDP `netPort`.
    bool netRelay = true;
    int netPort = 2105;
    // Joining: hosts whose games are listed even though their broadcasts don't arrive here
    // (comma-separated IPv4 addresses or host names, optionally with :port).
    std::string netHosts;
    // TCP_NODELAY on the game connection in both data flow modes; the game uses it for LAN only, MODEM/ISDN runs
    // with Nagle's algorithm (small messages wait for the previous one's ACK).
    bool netNoDelay = true;
    // Seconds a player connecting to a gameserver Sacred started has to send its first message (the game: 5).
    int netJoinTimeout = 30;

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
