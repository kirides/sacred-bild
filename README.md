# SacredBild

A `ddraw.dll` hook for **Sacred Gold** that runs the game at modern resolutions and instruments its
renderer, as groundwork for replacing the slow parts. It runs the game's DirectDraw / Direct3D 7 on Direct3D 9Ex
itself (or loads [DDrawCompat](https://github.com/narzoul/DDrawCompat) behind itself, `Backend=chain`), and patches
the game's internals (the approach of [GD3D11](https://github.com/kirides/GD3D11) for Gothic).

Supported executables: the **German** `sacred.exe` 2.0 (PE timestamp `0x451BBE74`) and the **English** GOG
`Sacred.exe` (`0x452F85C7`), with their `gameserver.exe`. SacredBild finds what it patches by byte signatures, not
fixed addresses, so other builds with the same code should work too; if a signature isn't found, it only passes
ddraw calls through and logs which one.

## What it does

- **Any resolution** (default: desktop resolution) instead of the hard-coded 1024x768:
  - back buffer and window at the target size, borderless window so the client area matches;
  - world view: orthographic projection (including its depth range), visible area, culling and
    overhead-label layout scaled so the world keeps its original pixel density and simply shows more
    (including the 0.5x-2.0x zoom);
  - world view centered on the screen (the game assumed a 512/384 screen center in 17 places);
  - UI: the game's 1024x768 UI (menus, HUD, cursor, intro videos) is drawn into a centered canvas,
    scaled to fit the screen height (`[UI] Scale`, `LinearFilter`); the mouse is mapped into that canvas
    for the UI while world picking keeps physical screen coordinates;
  - in game, the HUD windows go to the screen edges they had in the 1024x768 layout (`[UI] Anchor`): taskbar
    and chat at the bottom, inventory bottom-left, equipment bottom-right, minimap and character stats
    top-right, party portraits and the shop / chest / cube / trade windows top-left. Each keeps its own layout
    and runs in a shifted copy of the 1024x768 space (drawing, cursor, clicks, tooltips), so the game's code
    for it is unchanged; menus and full-screen windows (map, options, save) stay centered;
  - loading screen and splash (GDI) centered; savegame thumbnails taken from the screen center;
  - always a 32-bit display mode (`GFX32 : 0` in `Settings.cfg` is ignored).
- **Direct3D 9Ex backend** (`src/ddraw9/`): DirectDraw 7 and Direct3D 7 implemented on Direct3D 9Ex, for the
  subset Sacred uses (windowed swap chain, one render target with z-buffer, managed textures, system memory surfaces
  for GDI text, the fixed-function device). Both APIs are fixed-function with the same vertex formats and render
  states, so device calls translate almost one to one. Frames are presented with the flip model (`VSync`,
  `MaxFrameLatency`), and the per-call layers of Windows' Direct3D 7 runtime and DDrawCompat are gone. Anything the
  backend doesn't implement is logged once (`Direct3D 9 backend: not supported: ...`).
- **Movies through Media Foundation** (`[DDraw] MediaFoundation`, always with the Direct3D 9 backend): the game
  decodes its WMV movies with DirectShow into DirectDraw surfaces, which needs a real DirectDraw, and its loop stops
  processing window messages. SacredBild plays them with the Media Engine instead (audio included), draws them over
  the whole screen with their aspect ratio kept, and keeps the window responsive; ESC, space and mouse buttons skip
  as before.
- **Batched world rendering**: the world view issues thousands of small sprite and ground draws per frame
  (almost 10,000 zoomed out at 1920x1200). SacredBild records state changes instead of applying them and
  merges consecutive draws that end up with the same state into one call; small textures are copied into
  shared atlas pages so draws with different textures merge as well.
- **LAN games over VPNs**: Sacred finds LAN games through broadcasts that Windows sends on one network adapter
  only and that many VPNs don't carry, and the announced address is one Sacred picked from the first three
  adapters it found. When Sacred starts the gameserver for a hosted game, SacredBild goes along: every
  announcement goes out on every adapter with that adapter's own address, and to players who list the host in
  `[Net] Hosts` (see below).
- **Diagnostics**: per-second frame stats (draw calls, texture switches, unique textures, time spent in
  the world renderer, UI, flip and inside Direct3D, what ended each batch) and an optional sampling profiler.

## Build

Requires Visual Studio 2026 (MSVC, Win32 toolset), CMake 3.25+ and vcpkg with `VCPKG_ROOT` set
(Detours is pulled from vcpkg).

```sh
cmake --preset msvc-x86
cmake --build --preset release      # -> out/build/msvc-x86/RelWithDebInfo/ddraw.dll
python tools/check_hooks.py         # verifies hook signatures against the exe (German and English)
```

The DLL is statically linked against the CRT and imports only Windows system DLLs (`d3d9.dll` is loaded at run time).

## Install

```powershell
.\install.ps1 -GameDir "B:\Spiele\GOG Games\Sacred Gold"
```

This backs up the current `ddraw.dll`, moves an existing DDrawCompat to `SacredBild\DDrawCompat.dll`,
copies SacredBild's `ddraw.dll` and creates `SacredBild.ini`. `uninstall.ps1` restores the backup.
DDrawCompat is only used with `[DDraw] Backend=chain`; without it, that falls back to Windows' own `ddraw.dll`
(whose Direct3D 7 runtime refuses render targets over 2048 pixels; SacredBild works around that).

For development, symlink `ddraw.dll`/`ddraw.pdb` in the game folder to the build output instead.

## Configuration (`SacredBild.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| Display | Width, Height | 0 | Render resolution; 0 = desktop. 1024x768 = unpatched game. |
| Display | Borderless | 1 | Main window without frame. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh; 0 = right away. |
| Display | MaxFrameLatency | 1 | Direct3D 9 backend: frames the CPU may queue ahead of the GPU. |
| UI | Scale | 0 | UI scale; 0 = as large as fits the screen height, otherwise a factor (1 = native pixels), capped at that. |
| UI | LinearFilter | 1 | Bilinear filtering for the scaled UI instead of the game's point sampling. |
| UI | Anchor | 1 | In game, HUD windows at the screen edges and corners; 0 = all of the UI in the centered canvas. |
| Render | TextureBudgetMB | 0 | Texture memory the game may keep loaded; 0 = the game's value, at least 256. |
| Render | Batch | 1 | Merge the world view's draw calls. |
| Render | BatchNoClip | 1 | Merged draws skip Direct3D 7's software clipping; the GPU clips. |
| Render | BatchVertexBuffer | 1 | Merged draws go through vertex buffers instead of user memory. |
| Render | BatchModels | 1 | 3D model draws go through the batcher too (vertex buffers, merged where possible). |
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | Atlas | 1 | Copy small textures into shared pages so more draws merge. |
| Render | AtlasPageSize | 8192 | Atlas page size in texels (clamped to the GPU limit, halved if the GPU refuses it). |
| Render | AtlasPages | 2 | Pages per texture format; the least recently used one is reused when full. |
| Render | AtlasMaxTextureSize | 512 | Larger textures are used directly. |
| Net | Relay | 1 | Hosted games: the gameserver announces on every adapter and answers `Hosts` subscriptions. |
| Net | Port | 2105 | UDP port of that relay (host side). |
| Net | Hosts | | Hosts whose games are listed even without broadcasts: IPv4 addresses or names, comma-separated, optional `:port`. |
| Net | NoDelay | 1 | Game connection without Nagle's algorithm in both data flow modes (the game: LAN only). |
| Net | JoinTimeout | 30 | Seconds a joining player has to send its first message to a gameserver Sacred started (the game: 5). |
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 1 | Frame statistics in `SacredBild.log` (wraps the D3D device in a proxy). |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |

### LAN games over a VPN

Everyone installs SacredBild. The host creates the LAN game as usual; Windows Firewall has to let
`gameserver.exe` receive on the VPN adapter (the prompt on first start, or a rule for its TCP port and UDP 2105;
VPN adapters are often in the "Public" profile).

- VPNs that carry broadcasts (ZeroTier, Hamachi, Radmin VPN, OpenVPN TAP): the game shows up in the LAN list.
- VPNs without broadcasts (WireGuard, Tailscale, OpenVPN TUN): joining players add the host's VPN address,
  e.g. `Hosts=10.8.0.2` or a Tailscale name. A shared list of all players works, a PC skips its own addresses.

The host's relay logs to `SacredBild-server.log`, the LAN list to `SacredBild.log` (lines starting with `LAN`).

Read a profile with `python tools/profile_report.py <SacredBild-profile.txt>`.

## Layout

- `src/main.cpp`, `src/proxy.cpp`, `src/exports.def`: DLL entry, ddraw exports (to the Direct3D 9 backend or the
  chain-loaded ddraw). `src/system_ddraw.*`: render targets over 2048 pixels on Windows' own ddraw.
- `src/ddraw9/`: the Direct3D 9Ex backend: `directdraw.*` (IDirectDraw7 + IDirect3D7, exports), `surface.*`
  (IDirectDrawSurface7), `device.*` (IDirect3DDevice7), `vertex_buffer.*`, `gpu.*` (device, presentation),
  `format.*`. `d3d9_api.h` puts Direct3D 9 into namespace `d9`: its headers clash with Direct3D 7's.
- `src/game/sacred_addr.h`, `gameserver_addr.h`: the addresses SacredBild patches and the struct offsets it uses.
  The addresses are resolved at startup (`src/sig.*`) from `sacred_sigs.inc` / `gameserver_sigs.inc`, generated by
  `tools/gen_sigs.py`: signatures taken from the German build and accepted only if they match exactly once in
  the English one too (`SACRED_DE` / `SACRED_ENG` / `GAMESERVER_DE` / `GAMESERVER_ENG` point the tool at the exes).
- `src/game/resolution.*`, `resolution_sites.inc`: resolution patches (the `.inc` is generated by
  `tools/gen_res_sites.py`, which verifies every instruction and gives each site a signature and the value it
  must hold; both are checked again at startup).
- `src/game/ui_canvas.*`: UI canvas placement, UI scopes and frames, mouse mapping.
- `src/game/ui_anchor.*`: HUD windows anchored to the screen edges (frames per window, wrapped vtables, popups).
- `src/game/movie.*`: intro and cutscene movies (Media Foundation player for the Direct3D 9 backend).
- `src/game/device_proxy.*`: IDirect3DDevice7 wrapper; maps UI draws into the canvas, routes world draws
  through the batcher and instruments calls.
- `src/render/batcher.*`, `atlas.*`: deferred device state, draw merging and the texture atlas.
- `src/net/`: LAN games over VPNs: `lan_server.*` runs in gameserver.exe, `lan_client.*` in sacred.exe;
  `src/game/gameserver_de.h` holds the gameserver's addresses.
- `d3d_stats.*`, `frame_hooks.*`, `src/profiler.*`: instrumentation.
- `docs/RE_NOTES.md`: how the game's renderer works and where things are.
- `tools/`: Python helpers over the exe (`de.py`, `funcs.py`, `sigs.py`, `gen_sigs.py`, `gen_res_sites.py`,
  `check_hooks.py`, ...).
