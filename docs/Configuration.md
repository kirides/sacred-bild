# Configuration (`SacredBild.ini`)

Before the game starts, a settings window offers the settings below with tooltips: General (resolution, window frame,
frame limit, VSync, UI size, renderer), Advanced (everything else; the controller's buttons are set in game) and HUD layout (`[UI.Layout]`). Play saves the
changed ones to `SacredBild.ini`; Exit quits. "Don't show this window
again" sets `[Launcher] HideSettingsWindow=1`; holding Shift as the game starts shows it anyway.

A controller works the window too: the D-pad or left stick moves between the settings (left and right change a list's
value), A ticks a box, presses a button or opens a list, B closes an open list without changing it, LB and RB switch
tabs, Start is Play. Text fields need the keyboard.

| Section | Key | Default | Meaning |
|---|---|---|---|
| Launcher | HideSettingsWindow | 0 | 1 = no settings window before the game starts; holding Shift as the game starts shows it anyway. |
| Display | Width, Height | 0 | Render resolution; 0 = desktop. 1024x768 = unpatched game. |
| Display | Borderless | auto | Main window frame: `auto` = a frame when the window is smaller than the screen, `1` = never (the game's frameless window), `0` = always. The client area is `Width` x `Height` either way. |
| Display | Fullscreen | 0 | The window covers the whole screen at any `Width` x `Height`; the game keeps its resolution and the picture is scaled up to the screen. `1` / `fit` = at the picture's proportions with black bars, `2` / `stretch` = fills the screen. Overrides `Borderless` and `Resizable`; not in automation runs. See [Display](Display.md#fullscreen-at-a-lower-resolution). |
| Display | Resizable | 1 | A window with a frame can be resized and "maximized" (the largest window of the picture's proportions in the work area). The game keeps its resolution; the picture is stretched to the window. See [Display](Display.md#resizable-window). Not with `Borderless` windows or in automation runs. |
| Display | (automation) | | With `-gear-auto` the `Width`, `Height` and `Borderless` above are ignored: a framed 1600x900 window in the work area's bottom-right corner, `-gear-window=WxH[+X+Y]` changes it (see [Gear](Gear.md#automation)); `-gear-ui-scale=N` overrides `[UI] Scale` the same way. |
| Display | ClipCursor | 1 | Confine the mouse to the game window while it is in the foreground; hold Alt to move it out. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | FpsLimitMenu | 30 | Frame limit in the main menu (no game running); 0 = off. |
| Display | FpsLimitInactive | 20 | Frame limit while the game is in the background, in game and in the menus; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh (flip model); 0 = right away, not limited by the refresh rate (blit model). |
| Display | MaxFrameLatency | 1 | Direct3D 9 backend: frames the CPU may queue ahead of the GPU. |
| Camera | Zoom | Smooth | `Smooth`: any zoom between `ZoomMin` and `ZoomMax`, easing slightly into the target (numpad + / -, the controller's zoom buttons, the mouse wheel and the right stick). `Steps`: one step of `ZoomSteps` per press or wheel notch. `Original`: the game's own three steps (0.5, 1, 2). |
| Camera | ZoomMin | 0.5 | Closest zoom (0.2 to 1); the view is this fraction of the screen, so 0.35 shows the hero 1.4 times larger than the original's closest. |
| Camera | ZoomMax | 2 | Farthest zoom (1 to 2.5); far views draw many more tiles. |
| Camera | ZoomSteps | 0.5, 0.75, 1, 1.5, 2 | The zooms of `Zoom=Steps`, separated by commas. |
| Camera | ZoomSpeed | 100 | Percent: how fast a held zoom key and a wheel notch change the zoom. |
| Camera | ZoomWheel | 1 | The mouse wheel (and the controller's right stick) zooms while the cursor is over the world, not over a window. |
| UI | Scale | 0 | UI scale; 0 = as large as fits the screen height, otherwise a factor (1 = native pixels), capped at that. |
| UI | ScaleMode | InGame | Where `Scale` applies: `InGame` = the in-game UI only; the menus, the full-screen windows in game (options, save/load, character export, map) and the loading screen always fill the screen height. `Full` = those too. |
| UI | LinearFilter | 1 | Bilinear filtering for the scaled UI instead of the game's point sampling. |
| UI | Anchor | 1 | In game, HUD windows placed by `[UI.Layout]`; 0 = all of the UI in the centered canvas. |
| UI.Layout | Taskbar, Chat | 2048,4096 | Where a window's 1024x768 layout goes: X,Y in 0..4096 of the room the screen leaves around it (0 = left/top edge, 2048 = centered, 4096 = right/bottom edge). |
| UI.Layout | Inventory | 0,4096 | |
| UI.Layout | Stats, Equipment, Minimap | 4096,0 | |
| UI.Layout | Portraits, Shops | 0,0 | Party portraits; blacksmith, merchant, combat art master, chest, cube and trade. |
| Render | TextureBudgetMB | 0 | Texture memory the game may keep loaded; 0 = the game's value, at least 256. |
| Render | Batch | 1 | Merge the world view's draw calls. |
| Render | BatchNoClip | 1 | Merged draws skip Direct3D 7's software clipping; the GPU clips. |
| Render | BatchVertexBuffer | 1 | Merged draws go through vertex buffers instead of user memory. |
| Render | BatchModels | 1 | 3D model draws go through the batcher too (vertex buffers, merged where possible). |
| Render | GpuSkinning | 1 | Characters and their shadows skinned (animated) in a vertex shader instead of by Granny on the CPU; `Backend=d3d9` or `nvrhi`. |
| Render | OffscreenPoses | 4 | Skeletons of characters not drawn in the last frames are posed every Nth frame (staggered) instead of every frame; one drawn after all is posed before it is drawn. 1 = every frame. Needs `GpuSkinning`. |
| Render | SMAA | Off | Antialiasing of the world view with [SMAA](https://github.com/iryoku/smaa) 1x before the interface is drawn (the interface stays sharp): `Off`, `Low`, `Medium`, `High` (SMAA's presets: more edges found, a little more GPU time). The game itself has no antialiasing; its `FSAA_FILTER` option only keeps sprite edges from bleeding and works as before. `Backend=d3d9` (needs shader model 3) or `nvrhi`. |
| Render | TextureFilter | Point | Texture filtering of the game world: `Point` is the game's own sampling (sharp, blocky when zoomed in), `Sharp` keeps the texels crisp and blends only across the edge between two texels, over about one screen pixel ("sharp bilinear", good for pixel-art-like sprites and tiles at high resolutions; `Backend=d3d9` with shader support, draws it cannot reproduce stay point sampled, see the log), `Bilinear` is smoother when zoomed in but can show faint seams or dark fringes where neighbouring images of a texture sheet or colour-keyed edges blend in. With `Sharp` or `Bilinear` SacredBild switches the game's `FSAA_FILTER` (sprite texture coordinates pulled in by a fraction of a texel) on, whatever `Settings.cfg` says, so sprite pieces don't show a grid of thin lines. The interface follows `[UI] LinearFilter`. |
| Render | GroundMesh | 1 | The ground's tiles and blend layers kept in vertex buffers per sector, built once as they come into view, and drawn by a vertex shader in a few draws per frame; the game no longer rebuilds and draws every tile each frame. `Backend=d3d9` or `nvrhi`. |
| Render | BatchGround | 1 | The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls per quad. |
| Render | BatchSprites | 1 | The sprite batcher (water tiles, objects) hands its texture and quads to the batcher in one call instead of two device calls per flush (nearly every sprite). |
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | WarmFileCache | 1 | Read `PAK`, `World` and `mp3` once at startup in the background (low I/O priority, at most a quarter of the free memory, sound paks first), so the game's synchronous reads while playing come from Windows' file cache; a read from the idle SSD took ~8 ms on the render thread. |
| Render | FastMath | 1 | Faster replacements for the game's x87 math helpers with the same results: `__ftol` (float to integer) with SSE3's `fisttp`; the teleporter's ripple without x87 `fsin`/`fcos`. |
| Render | SoundLock | 1 | The sound system's lock as a user-mode lock (SRW lock) instead of a kernel mutex; the render thread takes it for every sound command, several per object and frame. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | AnimationAhead | 1 | With AsyncAnimation and a frame cap (FpsLimit or VSync): the frame's animation advance starts after the frame's interface, while the frame waits for its cap, so the next frame's models don't wait for it (poses one frame behind the game's clock). |
| Render | ParallelPosing | 1 | With AnimationThreads > 1: the skeletons are posed on those threads too, grouped by their root skeleton. |
| Render | AnimationThreads | 0 | Threads that sample Granny's animation controls in the advance, split by skeleton; 0 = automatic (CPU cores - 2, at most 4), 1 = Granny's own walk on one thread. |
| Render | Atlas | 1 | Copy small textures into shared pages so more draws merge. |
| Render | AtlasPageSize | 8192 | Atlas page size in texels (clamped to the GPU limit, halved if the GPU refuses it). |
| Render | AtlasPages | 2 | Pages per texture format; the least recently used one is reused when full. |
| Render | AtlasMaxTextureSize | 512 | Larger textures are used directly. |
| Screenshot | Format | png | Print Screen saves `Capture\shotNNNN.png`; `jpg` = JPEG (quality 95) instead. |
| Net | Relay | 1 | Hosted games: the gameserver announces on every adapter and answers `Hosts` subscriptions. |
| Net | Port | 2105 | Host side: UDP port of the relay, the UDP game connection and the matchmaker. |
| Net | Hosts | | Hosts whose games are listed even without broadcasts: IPv4 addresses or names, comma-separated, optional `:port`. |
| Net | NoDelay | 1 | Game connection without Nagle's algorithm in both data flow modes (the game: LAN only). |
| Net | JoinTimeout | 30 | Seconds a joining player has to send its first message to a gameserver Sacred started (the game: 5). |
| Net | Udp | 0 | Game connection over UDP when the other side has it on too (hosting: accept it); otherwise TCP. |
| Net | Prefer | IPv6 | Joining over UDP a host with IPv4 and IPv6 addresses: `IPv6` or `IPv4` is tried first, the other one as well after a second without an answer. |
| Net | Matchmaker | | Matchmaking server, name or address with optional `:port` (default 2107); empty = none. |
| Net | Publish | 1 | Hosting with a `Matchmaker`: the game is listed there. |
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below; `nvrhi` = SacredBild's own renderer on [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) (Direct3D 12 or Vulkan; experimental, see [Rendering](Rendering.md#nvrhi-backend-experimental)). |
| Render | Api | d3d12 | `Backend=nvrhi`: `d3d12` or `vulkan`; the other one is tried if a device can't be made, then the Direct3D 9 backend. |
| Render | WorldScale | 1 | `Backend=nvrhi`: the world view is drawn this many times the frame's size (up to 3) and scaled down before the interface goes on top (supersampling: smoother edges; costs GPU time). 1 = straight into the frame. |
| Render | TakeOverStatics | 1 | `Backend=nvrhi`: SacredBild draws the static objects of the world's draw lists (buildings, trees, props) itself instead of the game's code for them. 0 = the game's code, for comparing. |
| Render | GearGfx | 1 | `Backend=nvrhi`: Sacred Gear's UI is drawn with SacredBild's renderer directly (`bild_gfx.h`) instead of Direct3D 7 calls. 0 = the Direct3D 7 path, for comparing. |
| Render | BatchParticles | 1 | `Backend=nvrhi`: particles (a world matrix each) are moved into world space and merged into one draw per effect. 0 = one draw per particle, as before. |
| Render | MergeDraws | 1 | `Backend=nvrhi`: pretransformed draws with the same state as the one before (the interface's quads) are drawn as one. |
| Render | PipelineCache | 1 | `Backend=nvrhi`: the GPU pipelines are kept between runs in SacredBild-pipelines-d3d12.bin / -vulkan.bin next to the game; the file is discarded (and made again) when SacredBild, the GPU or its driver changed, or it is damaged. |
| Render | PlacedResources | 1 | `Backend=nvrhi`: textures are placed in shared memory blocks (D3D12 / Vulkan Memory Allocator) instead of each getting memory of its own; making one takes ~0.05 ms instead of ~0.35-0.4 ms (loading, textures streamed in while playing). |
| Render | SpriteFilter | 0 | `Backend=nvrhi`: the world's point-sampled 2D art (ground tiles, sprites) is magnified with an edge-directed filter in the manner of xBR: smooth diagonals instead of stair steps when zoomed in. |
| Debug | RhiValidation | 0 | `Backend=nvrhi`: the API's validation (Direct3D 12 debug layer, Vulkan's `VK_LAYER_KHRONOS_validation`) and NVRHI's, messages in the log; slow. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | D3D9 | | `Backend=d3d9`: `d3d9.dll` to use (e.g. DXVK), relative to the game folder or absolute. Empty or not loadable: a `d3d9.dll` next to the exe, then Windows' own. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 0 | Frame statistics, hitches and texture memory in `SacredBild.log`, and the world view's time by pass (`passes:` lines); with `Backend=nvrhi` also the GPU time a frame and textures made (and the time making them), read-backs and GPU waits a frame (`NVRHI backend: GPU` lines). |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |
| Debug | ProfilerWorldOnly | 0 | The profile leaves out start-up and loading: what was sampled before the world was drawn is dropped two seconds after its first frame. |
| Debug | UiTrace | 0 | Scroll Lock logs one UI frame's draws (`UiTrace:` lines: position, UI frame, calling game code) and popups set during the next 5 s. |
| Debug | CrashDump | 1 | Minidump next to the exe on a crash: 0 = off, 1 = stacks and the memory they point to (small), 2 = all memory (for the game's globals; hundreds of MB). |
| Debug | SkinCheck | 0 | Compares Granny's CPU skinning of characters with per-vertex bone weights rebuilt from its meshes (`Skin check:` lines every 10 s): groundwork for skinning on the GPU. |
| Debug | AnimationCheck | 0 | With `AnimationThreads`: every 2 s the controls are sampled on one thread and on the threads from the same state and compared (`Animation threads:` lines); a difference keeps the walk on one thread. |
| Debug | MovieFallback | 0 | Movies always through the fallback (DirectShow into a system memory surface), as without the Media Engine. |

The controller's settings and bindings (`[Controller]`, `[Controller.Bindings]`) are in [Controller](Controller.md#settings).
