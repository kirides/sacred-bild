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
| Display | ClipCursor | 1 | Confine the mouse to the game window while it is in the foreground; hold Alt to move it out. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | FpsLimitInactive | 20 | Frame limit while the game is in the background, in game and in the menus; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh (flip model); 0 = right away, not limited by the refresh rate (blit model). |
| Display | MaxFrameLatency | 1 | Direct3D 9 backend: frames the CPU may queue ahead of the GPU. |
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
| Render | GpuSkinning | 1 | Characters and their shadows skinned (animated) in a vertex shader instead of by Granny on the CPU; `Backend=d3d9` only. |
| Render | OffscreenPoses | 4 | Skeletons of characters not drawn in the last frames are posed every Nth frame (staggered) instead of every frame; one drawn after all is posed before it is drawn. 1 = every frame. Needs `GpuSkinning`. |
| Render | GroundMesh | 0 | Experimental: the ground's tiles and blend layers kept in vertex buffers per sector, built once as they come into view, and drawn by a vertex shader in a few draws per frame; the game no longer rebuilds and draws every tile each frame. `Backend=d3d9` only. |
| Render | BatchGround | 1 | The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls per quad. |
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | AnimationThreads | 1 | Experimental: threads that sample Granny's animation controls in the advance, split by skeleton; 1 = Granny's own walk, 0 = automatic (CPU cores - 2, at most 4). |
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
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | D3D9 | | `Backend=d3d9`: `d3d9.dll` to use (e.g. DXVK), relative to the game folder or absolute. Empty or not loadable: a `d3d9.dll` next to the exe, then Windows' own. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 0 | Frame statistics, hitches and texture memory in `SacredBild.log`, and the world view's time by pass (`passes:` lines). |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |
| Debug | UiTrace | 0 | Scroll Lock logs one UI frame's draws (`UiTrace:` lines: position, UI frame, calling game code) and popups set during the next 5 s. |
| Debug | CrashDump | 1 | Minidump next to the exe on a crash: 0 = off, 1 = stacks and the memory they point to (small), 2 = all memory (for the game's globals; hundreds of MB). |
| Debug | SkinCheck | 0 | Compares Granny's CPU skinning of characters with per-vertex bone weights rebuilt from its meshes (`Skin check:` lines every 10 s): groundwork for skinning on the GPU. |
| Debug | AnimationCheck | 0 | With `AnimationThreads`: every 2 s the controls are sampled on one thread and on the threads from the same state and compared (`Animation threads:` lines); a difference keeps the walk on one thread. |
| Debug | MovieFallback | 0 | Movies always through the fallback (DirectShow into a system memory surface), as without the Media Engine. |

The controller's settings and bindings (`[Controller]`, `[Controller.Bindings]`) are in [Controller](Controller.md#settings).
