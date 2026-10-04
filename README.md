# SacredBild

> **AI disclosure:** SacredBild is developed with an AI coding assistant (Anthropic's Claude). Most of the code,
> the reverse engineering notes and this documentation were written with it, at the maintainer's direction; the
> maintainer decides what goes in and tests the changes in the game. The source is kept to what the features need
> and documented so that people can review it: see [Reading the code](#reading-the-code).

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
  - back buffer and window at the target size: borderless when it fills the screen, otherwise with a frame
    (caption and system menu: it can be moved and minimized) around a client area of that size (`[Display] Borderless`);
    its close button is disabled so a stray click doesn't end the game (Alt+F4 still quits);
  - world view: orthographic projection (including its depth range), visible area, culling and
    overhead-label layout scaled so the world keeps its original pixel density and simply shows more
    (including the 0.5x-2.0x zoom);
  - world view centered on the screen (the game assumed a 512/384 screen center in 17 places);
  - UI: the game's 1024x768 UI (menus, HUD, cursor, intro videos) is drawn into a centered canvas,
    scaled to fit the screen height (`[UI] Scale`, `ScaleMode`, `LinearFilter`); the mouse is mapped into that canvas
    for the UI while world picking keeps physical screen coordinates;
  - in game, the HUD windows are placed on the screen (`[UI] Anchor`, positions in `[UI.Layout]`); by default
    at the edges they had in the 1024x768 layout: taskbar and chat at the bottom, inventory bottom-left, minimap,
    character stats and equipment top-right, party portraits and the shop / chest / cube / trade windows
    top-left. Each keeps its own layout and runs in a shifted copy of the 1024x768 space (drawing, cursor, clicks,
    tooltips), so the game's code for it is unchanged; menus and full-screen windows (map, options, save) stay
    centered. Tooltips and the item on the cursor use the whole screen, the help screen's texts (H key) stay next
    to the windows they explain, and the escape menu and message boxes dim the whole screen;
  - loading screen (GDI) drawn into a 1024x768 surface and scaled like the menus, splash centered; savegame
    thumbnails taken from the screen center;
  - always a 32-bit display mode (`GFX32 : 0` in `Settings.cfg` is ignored);
  - screenshots (Print Screen) of the whole screen as `Capture\shotNNNN.png` (or `.jpg`, `[Screenshot] Format`),
    saved on a worker thread; the game's own wrote the top-left 1024x768 as TGA + JPEG, sheared at other widths.
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
  as before. Where the Media Engine is missing (Windows 7, N editions) or can't play a movie (Wine), they are
  decoded with the game's DirectShow streams into a system memory surface of Windows' own DirectDraw instead, and
  drawn the same way (`[Debug] MovieFallback`).
- **Batched world rendering**: the world view issues thousands of small sprite and ground draws per frame
  (almost 10,000 zoomed out at 1920x1200). SacredBild records state changes instead of applying them and
  merges consecutive draws that end up with the same state into one call; small textures are copied into
  shared atlas pages so draws with different textures merge as well.
- **LAN games over VPNs**: Sacred finds LAN games through broadcasts that Windows sends on one network adapter
  only and that many VPNs don't carry, and the announced address is one Sacred picked from the first three
  adapters it found. When Sacred starts the gameserver for a hosted game, SacredBild goes along: every
  announcement goes out on every adapter with that adapter's own address, and to players who list the host in
  `[Net] Hosts` (see below).
- **Languages**: the GOG builds ignore the game's `LANGUAGE` for text and speech (the text is built into the exe,
  the speech is always `PAK\sound.pak`). SacredBild loads the language's own files when they are there, so one
  install runs every language you have the files for (see below).
- **Input only in the foreground**: the game polls keys and mouse buttons whether or not it has the focus, moves
  the cursor, and its low-level keyboard hook swallowed the Windows keys, Alt+Tab and Alt+Esc system-wide. All of
  that now only happens while the game is in the foreground (its cursor still follows the mouse over the window, so
  you see where a click will land), so typing in another window doesn't move your character, and Alt+Tab / Alt+Esc work in
  game too (the Windows keys and Ctrl+Esc stay blocked while it has the focus). The mouse is confined to the
  game window while it is in the foreground (`[Display] ClipCursor`), e.g. a window on one side of a 32:9 screen
  or a borderless game next to a second monitor; it is let go while the window is moved (Alt+Space, Move) or a
  menu of it is open, and while Alt is held; it is only taken once the cursor is over the window, so a click on the
  title bar still drags.
- **Rendering in the background**: the game stopped drawing when it lost the focus, which often left the main menu
  black. It now keeps drawing at `[Display] FpsLimitInactive` frames per second.
- **Diagnostics**: optional per-second frame stats (`[Debug] D3DStats`: draw calls, texture switches, unique
  textures, time spent in the world renderer, UI, flip and inside Direct3D, what ended each batch, texture memory)
  and an optional sampling profiler (`[Debug] Profiler`).
  When the game or its gameserver crashes, a minidump goes next to the exe (`SacredBild-crash-*.dmp`,
  `SacredBild-server-crash-*.dmp`; `[Debug] CrashDump`) and the log names the exception and where it happened.

## Install

From a [release](../../releases) (tagged versions, and a nightly prerelease of every push to `main`): extract the
zip into the game folder. It replaces the `ddraw.dll` there (if that is DDrawCompat, move it to
`SacredBild\DDrawCompat.dll` first) and an existing `SacredBild.ini`.

From a build (see [Build](#build)):

```powershell
.\install.ps1 -GameDir "B:\Spiele\GOG Games\Sacred Gold"
```

This backs up the current `ddraw.dll`, moves an existing DDrawCompat to `SacredBild\DDrawCompat.dll`,
copies SacredBild's `ddraw.dll` and creates `SacredBild.ini` if there is none. `uninstall.ps1` restores the backup.
DDrawCompat is only used with `[DDraw] Backend=chain`; without it, that falls back to Windows' own `ddraw.dll`
(whose Direct3D 7 runtime refuses render targets over 2048 pixels; SacredBild works around that).

The Direct3D 9 backend runs on any `d3d9.dll`: `[DDraw] D3D9`, else a `d3d9.dll` next to the game exe (e.g.
[DXVK](https://github.com/doitsujin/dxvk)'s 32-bit one), else Windows' own. `SacredBild.log` names the one in use
(`Direct3D 9: using ...`). Under Wine / Proton, SacredBild's `ddraw.dll` only loads with a native override:
`WINEDLLOVERRIDES="ddraw=n,b"`.

## Configuration (`SacredBild.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| Display | Width, Height | 0 | Render resolution; 0 = desktop. 1024x768 = unpatched game. |
| Display | Borderless | auto | Main window frame: `auto` = a frame when the window is smaller than the screen, `1` = never (the game's frameless window), `0` = always. The client area is `Width` x `Height` either way. |
| Display | ClipCursor | 1 | Confine the mouse to the game window while it is in the foreground; hold Alt to move it out. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | FpsLimitInactive | 20 | Frame limit while the game is in the background, in game and in the menus; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh; 0 = right away. |
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
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | Atlas | 1 | Copy small textures into shared pages so more draws merge. |
| Render | AtlasPageSize | 8192 | Atlas page size in texels (clamped to the GPU limit, halved if the GPU refuses it). |
| Render | AtlasPages | 2 | Pages per texture format; the least recently used one is reused when full. |
| Render | AtlasMaxTextureSize | 512 | Larger textures are used directly. |
| Screenshot | Format | png | Print Screen saves `Capture\shotNNNN.png`; `jpg` = JPEG (quality 95) instead. |
| Net | Relay | 1 | Hosted games: the gameserver announces on every adapter and answers `Hosts` subscriptions. |
| Net | Port | 2105 | UDP port of that relay (host side). |
| Net | Hosts | | Hosts whose games are listed even without broadcasts: IPv4 addresses or names, comma-separated, optional `:port`. |
| Net | NoDelay | 1 | Game connection without Nagle's algorithm in both data flow modes (the game: LAN only). |
| Net | JoinTimeout | 30 | Seconds a joining player has to send its first message to a gameserver Sacred started (the game: 5). |
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | D3D9 | | `Backend=d3d9`: `d3d9.dll` to use (e.g. DXVK), relative to the game folder or absolute. Empty or not loadable: a `d3d9.dll` next to the exe, then Windows' own. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 0 | Frame statistics, hitches and texture memory in `SacredBild.log`. |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |
| Debug | UiTrace | 0 | Scroll Lock logs one UI frame's draws (`UiTrace:` lines: position, UI frame, calling game code) and popups set during the next 5 s. |
| Debug | CrashDump | 1 | Minidump next to the exe on a crash: 0 = off, 1 = stacks and the memory they point to (small), 2 = all memory (for the game's globals; hundreds of MB). |
| Debug | MovieFallback | 0 | Movies always through the fallback (DirectShow into a system memory surface), as without the Media Engine. |

## LAN games over a VPN

Everyone installs SacredBild. The host creates the LAN game as usual; Windows Firewall has to let
`gameserver.exe` receive on the VPN adapter (the prompt on first start, or a rule for its TCP port and UDP 2105;
VPN adapters are often in the "Public" profile).

- VPNs that carry broadcasts (ZeroTier, Hamachi, Radmin VPN, OpenVPN TAP): the game shows up in the LAN list.
- VPNs without broadcasts (WireGuard, Tailscale, OpenVPN TUN): joining players add the host's VPN address,
  e.g. `Hosts=10.8.0.2` or a Tailscale name. A shared list of all players works, a PC skips its own addresses.

The host's relay logs to `SacredBild-server.log`, the LAN list to `SacredBild.log` (lines starting with `LAN`).

## Languages

Sacred takes its language from `LANGUAGE` in `settings.cfg`, one of `US DE FR SP IT PL HU JP VC RU CZ` in upper
case (anything else quietly means `DE`), or from the code in lower case on the command line (`sacred.exe de`).
Both GOG installs ship with `LANGUAGE : US`, whatever their language. SacredBild uses the language's files if they
are there, and otherwise what the build ships with:

- text: `scripts\<code>\global.res`, else the text built into the exe;
- speech: `PAK\sound.<code>.pak`, else `PAK\sound.pak`.

`import-language.ps1` takes the text out of another install's exe, copies its `PAK\sound.pak` and stores both
under a code, e.g. German into the English install:

```powershell
.\import-language.ps1 -From "B:\Spiele\GOG Games\Sacred Gold" -Language DE -GameDir "B:\Spiele\GOG Games\sacred gold GOG"
```

Then set `LANGUAGE : DE`. `-HardLink` links `sound.pak` instead of copying it (400-470 MB, same drive only),
`-NoSpeech` takes only the text. A GOG install's `scripts\us\global.res` is a copy of its built-in text, so
`LANGUAGE : US` keeps the install's language until you import another one as `US`. To add English to the German
install, import it as `US`; `LANGUAGE : DE` then still gives German (no `scripts\de` and no `sound.de.pak`, so the
built-in text and `PAK\sound.pak`). `SacredBild.log` names the files in use (lines starting with `Language:`).

The code also changes a few things in the game: keyboard input for `PL`, IME and line breaking for `JP` and `VC`,
no status text on the loading screen for `SP`. Savegames store it; loading one saved under another code only logs
the difference.

## Reading the code

SacredBild is one DLL, `ddraw.dll`, which the game loads from its folder in place of Windows' own. Start in
`src/main.cpp`: `DllMain` reads `SacredBild.ini` (`src/config.*`), loads the DirectDraw implementation the game
will get (`src/proxy.*`), finds the game's code by byte signature (`Sacred::resolveAddresses`) and installs the
patches (`Sacred::installHooks` in `src/game/build.cpp`, which lists every module in order). Each module patches
the game in its `install()` with [Detours](https://github.com/microsoft/Detours) hooks or checked byte patches
(`src/patch.*`: a write happens only if the bytes there are the expected ones).

What the game's code at those addresses does is in `docs/RE_NOTES.md` and in the comments of
`src/game/sacred_addr.h`; the addresses and names refer to the German build in Ghidra.

### Source files

| Path | What it is |
|---|---|
| `src/main.cpp` | DLL entry: game (`sacred.exe`) or LAN gameserver (`gameserver.exe`) setup |
| `src/proxy.*`, `src/exports.def` | The `ddraw.dll` exports, to the Direct3D 9 backend or the chain-loaded ddraw |
| `src/system_ddraw.*` | `Backend=chain` on Windows' own ddraw: render targets over 2048 pixels |
| `src/config.*` | `SacredBild.ini` |
| `src/log.*`, `src/crash_dump.*` | `SacredBild.log`, minidumps on crashes |
| `src/patch.*`, `src/sig.*` | Code patches and hooks; byte signature search |
| `src/spin_lock.h` | Locks for the render thread's hot paths |
| `src/profiler.*` | `[Debug] Profiler` |
| `src/ddraw9/` | The Direct3D 9Ex backend: `directdraw.*` (IDirectDraw7, IDirect3D7, the exports), `surface.*` (IDirectDrawSurface7), `device.*` (IDirect3DDevice7), `vertex_buffer.*`, `gpu.*` (the Direct3D 9 device, presentation), `format.*` (pixel formats); `d3d9_api.h` puts Direct3D 9 into namespace `d9`, its headers clash with Direct3D 7's |
| `src/game/build.*` | Which exe this is, address lookup, the list of game modules |
| `src/game/sacred_addr.h`, `sacred_sigs.inc` | The sacred.exe addresses and struct offsets SacredBild uses; their signatures (generated) |
| `src/game/gameserver*` | The same for gameserver.exe, and its patches |
| `src/game/resolution.*`, `resolution_sites.inc` | Any resolution: window, back buffer, world view; the 1024x768 constants patched (generated) |
| `src/game/ui_canvas.*` | The 1024x768 UI in a scaled, centered canvas; cursor mapping |
| `src/game/ui_anchor.*` | HUD windows placed at the screen edges (`[UI] Anchor`, `[UI.Layout]`) |
| `src/game/device_proxy.*` | The IDirect3DDevice7 the game gets: UI draws into the canvas, world draws to the batcher |
| `src/game/frame_hooks.*` | Device creation, flip, world and UI render; frame limits |
| `src/game/focus.*` | Input only in the foreground, `ClipCursor`, close button |
| `src/game/movie.*` | Intro and cutscene movies through Media Foundation |
| `src/game/screenshot.*` | Print Screen as PNG / JPEG |
| `src/game/language.*` | Text and speech files of the game's language |
| `src/game/granny_async.*` | Animation update on a worker thread (`[Render] AsyncAnimation`) |
| `src/game/map_cache.*` | Hash index in front of the map record caches (`[Render] RecordIndex`) |
| `src/game/d3d_stats.*` | `[Debug] D3DStats` frame statistics |
| `src/render/batcher.*`, `atlas.*`, `fvf.h` | Draw merging, the texture atlas, vertex format layout |
| `src/net/` | LAN games over VPNs: `lan_client.*` (sacred.exe), `lan_server.*` (gameserver.exe), `lan_protocol.*` (announcements), `adapters.*` (network adapters), `connection.*` (`NoDelay`) |

Every header starts with what its module does and why.

### Tools

Python 3 with `pefile` and `capstone`. They read the game's exes from the paths in `tools/gen_sigs.py`, or from
the environment variables `SACRED_DE`, `SACRED_ENG`, `GAMESERVER_DE` and `GAMESERVER_ENG`.

Generated files in the repository, and the checks:

| Tool | Does |
|---|---|
| `tools/gen_sigs.py` | Writes `src/game/sacred_sigs.inc`, `gameserver_sigs.inc` and `tools/data/addresses.json`: a signature for every address, taken from the German build and accepted only if it matches exactly once in the English one too |
| `tools/gen_res_sites.py` | Writes `src/game/resolution_sites.inc`: the 1024x768 constants to patch, each verified and with a signature |
| `tools/check_hooks.py` | Checks that every hook declares as many stack arguments as the hooked function pops, in both builds |
| `tools/sigs.py`, `funcs.py`, `de.py` | Shared code: signature search, function boundaries (`tools/data/functions_de.tsv`, exported from Ghidra), the German exe |

Reverse engineering helpers (how addresses and offsets were found):

| Tool | Does |
|---|---|
| `tools/de.py <address> [count]` | Disassembles the German exe |
| `tools/rtti.py [regex]` | Class names -> vtables, from the RTTI |
| `tools/vt.py <vtable>...` | Vtable slots |
| `tools/scan_res.py` | Every instruction that uses a 1024x768-related constant |
| `tools/check_rets.py <address>...` | `ret` sizes, i.e. stack argument counts |
| `tools/profile_report.py <SacredBild-profile.txt>` | Names the hot spots of a `[Debug] Profiler` profile |

### Build

Requires Visual Studio 2026 (MSVC, Win32 toolset), CMake 3.25+ and vcpkg with `VCPKG_ROOT` set
(vcpkg provides [Detours](https://github.com/microsoft/Detours) and [gtl](https://github.com/greg7mdp/gtl)).

```sh
cmake --preset msvc-x86
cmake --build --preset release      # -> out/build/msvc-x86/RelWithDebInfo/ddraw.dll
python tools/check_hooks.py         # verifies hook signatures against the exe (German and English)
```

The DLL is statically linked against the CRT and imports only Windows system DLLs (`d3d9.dll` is loaded at run time).
For development, symlink `ddraw.dll`/`ddraw.pdb` in the game folder to the build output instead of installing.
`.github/workflows/` builds every push and pull request; pushes to `main` publish a nightly prerelease, `v*` tags a
release.
