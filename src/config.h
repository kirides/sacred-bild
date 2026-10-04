#pragma once
#include <string>

// Settings from SacredBild.ini next to the game exe.
struct Config
{
    // DirectDraw / Direct3D 7 on SacredBild's own Direct3D 9Ex backend (Backend=d3d9), or on the chain-loaded ddraw
    // (Backend=chain): `ddrawChain`, or the system ddraw.dll if that is empty or missing.
    bool ddrawD3D9 = true;
    std::wstring ddrawChain = L"SacredBild\\DDrawCompat.dll";
    // Backend=d3d9: d3d9.dll to load first (e.g. DXVK), relative to the game folder or absolute. Then a d3d9.dll
    // next to the exe, then the system's.
    std::wstring d3d9Path;
    // Movies through Media Foundation instead of the game's DirectShow/DirectDraw path. Backend=d3d9 always does
    // (DirectShow can't decode into its surfaces); with Backend=chain this decides.
    bool mediaFoundation = true;

    // Render resolution; 0 = desktop size. 1024x768 runs the game unpatched.
    int width = 0;
    int height = 0;
    // Main window frame ([Display] Borderless): Auto = a frame (caption, system menu) when the window is smaller than
    // the screen, Never (Borderless=1) = the game's frameless popup, Always (Borderless=0). The client area is the
    // render resolution either way.
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

    // UI canvas: the 1024x768 UI drawn centered. 0 = scale to fit the screen height.
    float uiScale = 0.0f;
    // ScaleMode=Full: uiScale applies to the menus (start menu, options, ...) too. InGame: in game only, the menus
    // always fit the screen height.
    bool uiScaleMenus = false;
    // In game, the HUD windows (taskbar, minimap, inventory, ...) are placed on the screen by [UI.Layout] instead
    // of staying in the centered canvas.
    bool uiAnchor = true;
    // [UI.Layout]: where each in-game window's 1024x768 layout goes, as X,Y in 0..4096 of the room the screen leaves
    // around it: 0 = against the left/top edge, 2048 = centered, 4096 = against the right/bottom edge. Windows at
    // the same position keep their 1024x768 arrangement.
    struct UiPosition
    {
        int x, y;
    };
    UiPosition uiTaskbar{2048, 4096};
    UiPosition uiChat{2048, 4096};
    UiPosition uiInventory{0, 4096};
    UiPosition uiEquipment{4096, 0};
    UiPosition uiStats{4096, 0};
    UiPosition uiMinimap{4096, 0};
    UiPosition uiPortraits{0, 0};
    UiPosition uiShops{0, 0};       // blacksmith, merchant, combat art master, chest, cube, trade
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
    // The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls.
    bool batchGround = true;
    // Characters skinned in a vertex shader instead of by Granny on the CPU (Direct3D 9 backend only).
    bool gpuSkinning = false;
    // GrannyAdvanceTime runs on a worker thread, overlapping the start of the frame.
    bool asyncAnimation = true;
    // Hash index in front of the map data's record caches (std::map lookups per tile and object).
    bool recordIndex = true;
    // Copy small textures into shared pages so draws with different textures can be merged as well.
    bool atlas = true;
    int atlasPageSize = 8192;       // texels per side, clamped to the device limit
    int atlasPages = 2;             // at most this many pages per texture format
    int atlasMaxTextureSize = 512;  // larger textures are used directly

    // Screenshots (Print Screen) of the whole screen as Capture\shotNNNN.png, or .jpg with Format=jpg.
    bool screenshotJpeg = false;

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
    bool d3dStats = false;         // per-second D3D7 call counts in the log
    bool profiler = false;        // sample the render thread, dump hot spots on exit
    bool uiTrace = false;         // Scroll Lock logs one UI frame's draws with their frames and callers
    // Minidump next to the exe when the game crashes: 0 = off, 1 = stacks and the memory they point to, 2 = all memory.
    int crashDump = 1;
    // Movies always through the fallback (DirectShow into a system memory surface) instead of Media Foundation.
    bool movieFallback = false;
    // Compare Granny's CPU skinning with per-vertex weights (groundwork for skinning on the GPU).
    bool skinCheck = false;
    int profilerIntervalUs = 500;
};

extern Config g_config;

namespace ConfigFile
{
    void load(const std::wstring& gameDir);
}
