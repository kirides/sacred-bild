# Development

SacredBild is one DLL, `ddraw.dll`, which the game loads from its folder in place of Windows' own. Start in
`src/bild/main.cpp`: `DllMain` reads `SacredBild.ini` (`src/bild/settings/config.*`); with the settings window, the rest waits for the
exe's entry point, after the window (`src/bild/settings/settings_window.*`). Then it loads the DirectDraw implementation the game
will get (`src/bild/proxy.*`), finds the game's code by byte signature (`Sacred::resolveAddresses`) and installs the
patches (`Sacred::installHooks` in `src/bild/engine/build.cpp`, which lists every module in order). Each module patches
the game in its `install()` with [Detours](https://github.com/microsoft/Detours) hooks or checked byte patches
(`src/core/patch.*`: a write happens only if the bytes there are the expected ones).

What the game's code at those addresses does is in [`RE_NOTES.md`](RE_NOTES.md), in the comments of
`src/bild/engine/sacred_addr.h` and in the game's classes in `src/bild/sacred/`; the addresses refer to the English GOG build
(Ghidra program `/Sacred.exe (GOG)`).

## Source layout

All code is under `src/`, one folder per DLL plus the code both link, and at most one folder level below each:

| Folder | What it holds |
|---|---|
| `src/core/` | `sacred_core`, linked by both DLLs: log, formatting, patches and hooks, signature search, instances, `sacred/address.h` (the typed addresses), `bild_gear.h` (the exports the two DLLs call on each other). Counts for both versions |
| `src/bild/` | `ddraw.dll`: entry, proxy and exports at the root; `engine/` (sacred.exe and gameserver.exe addresses, module list), `sacred/` (the game's classes), `settings/` (ini, settings window, `SacredBild.ini`), `ddraw9/` (Direct3D 9 backend), `render/`, `perf/`, `display/`, `ui/`, `controller/`, `overlay/`, `net/`, `game/`, `diag/` |
| `src/gear/` | `SacredGear.dll`: the Lua host and what every area uses at the root (with the natives of each area in a `*_lua.cpp` next to its module), `world/`, `items/`, `ui/`, `engine/` (sacred.exe addresses and the generated tables), `lua/` (the kernel in parts listed in `kernel.parts`, the libraries), `meta/` (the Lua API annotations), `mdk/`, `examples/`, `tests/`, `res/` |

The repository paths every tool uses are in `tools/paths.py`. `tools/relayout.py <stage>` holds the moves that made this
layout (stages `core`, `bild`, `gear`, `tools`) with the include and path rewrite; run a stage again on a branch cut
before it (after rebasing) to move the files the branch added.

## Source files

| Path | What it is |
|---|---|
| `src/bild/main.cpp` | DLL entry: game (`sacred.exe`) or LAN gameserver (`gameserver.exe`) setup |
| `src/bild/proxy.*`, `src/bild/exports.def` | The `ddraw.dll` exports, to the Direct3D 9 backend or the chain-loaded ddraw |
| `src/bild/system_ddraw.*` | `Backend=chain` on Windows' own ddraw: render targets over 2048 pixels |
| `src/bild/settings/config.*`, `src/bild/settings/config/` | `SacredBild.ini`: loading it, and the settings in a header per ini section (`Config::render`, `Config::net`, ...) |
| `src/bild/settings/settings_window.*` | The settings window before the game starts (`[Launcher] HideSettingsWindow`): what it shows, declared control by control |
| `src/bild/settings/form.*` | `Ui::Form`: settings windows declared in code (pages, groups, rows of controls bound to ini keys), laid out and run as dialogs |
| `src/core/log.*`, `src/bild/crash_dump.*` | `SacredBild.log`, minidumps on crashes |
| `src/core/patch.*`, `src/core/sig.*` | Code patches and hooks; byte signature search |
| `sacred_core` (CMake) | `src/core/fmt.*`, `log.*`, `patch.*`, `sig.*` as a static library, linked by SacredBild and by Sacred Gear (`src/gear/`, see [Sacred Gear](Gear.md)) |
| `src/bild/spin_lock.h` | Locks for the render thread's hot paths |
| `src/bild/mem.h` | Fields of granny.dll's objects by byte offset (`Mem::field`) |
| `src/bild/diag/profiler.*` | `[Debug] Profiler` |
| `src/bild/ddraw9/` | The Direct3D 9Ex backend: `directdraw.*` (IDirectDraw7, IDirect3D7, the exports), `surface.*` (IDirectDrawSurface7), `device.*` (IDirect3DDevice7), `vertex_buffer.*`, `device_skin.cpp` / `device_ground.cpp` (the skinning and ground shaders, `skin.hlsl`, `ground.hlsl`), `smaa.*` (SMAA antialiasing, `smaa.hlsl` over the reference shader and lookup textures in `smaa/`), `sharp.hlsl` (the pixel shader of `[Render] TextureFilter=Sharp`, bound by `Device::updateSharp` for draws whose stage 0/1 combiner it reproduces), `gpu.*` (the Direct3D 9 device, presentation), `format.*` (pixel formats); `d3d9_api.h` puts Direct3D 9 into namespace `d9`, its headers clash with Direct3D 7's |
| `src/bild/engine/build.*` | Which exe this is, address lookup, the list of game modules |
| `src/bild/engine/sacred_addr.h`, `sacred_sigs.inc` | The sacred.exe addresses SacredBild uses, functions with their signatures (`Sacred::Thiscall`, `Cdecl`; `tools/sigs/check_hooks.py` checks them) and globals with their types (`Sacred::Global`); their signatures (generated) |
| `src/bild/sacred/` | The game's classes as structs (the Sacred Gold API): fields at their offsets and vtables as structs of function pointers, both checked by `static_assert`, member functions calling the game's code. `engine.h` (cEngine, orders), `object.h` (cObject / cCreature, cObjectManager), `world.h` (cWorldView, sectors, tiles, map data), `render.h` (dxDriver7, textures, cQuadBatcher), `ui.h` (cUI_Control2 / cUI_Window2 and their subclasses, cUI_Manager, events, fonts), `mouse.h`, `text.h`, `net.h` (cGCclass; gameserver.exe's cApp / cNetServer); `address.h` the typed addresses, `vector.h` the exe's std::vector |
| `src/bild/net/gameserver.*`, `src/bild/engine/gameserver_*` | The same for gameserver.exe, and its patches |
| `src/bild/display/resolution.*`, `resolution_sites.inc` | Any resolution: window, back buffer, world view; the 1024x768 constants patched (generated) |
| `src/bild/ui/ui_state.*` | What the UI manager shows and who gets the input, by its own rules (mode, windows drawn alone, HUD, message box, focus); see `docs/UI_SYSTEM.md` |
| `src/bild/ui/ui_windows.*` | The manager's windows, menus, message box, popups and text lines with their virtual functions wrapped: which one runs on a thread (`current()`), frames per object for UiAnchor |
| `src/bild/ui/ui_canvas.*` | The 1024x768 UI in a scaled, centered canvas; cursor mapping |
| `src/bild/ui/ui_anchor.*` | HUD windows placed at the screen edges (`[UI] Anchor`, `[UI.Layout]`) |
| `src/bild/render/device_proxy.*` | The IDirect3DDevice7 the game gets: UI draws into the canvas, world draws to the batcher |
| `src/bild/render/frame_hooks.*` | Device creation, flip, world and UI render; frame limits |
| `src/bild/display/focus.*` | Input only in the foreground, `ClipCursor`, close button |
| `src/bild/game/movie.*` | Intro and cutscene movies through Media Foundation |
| `src/bild/game/screenshot.*` | Print Screen as PNG / JPEG |
| `src/bild/game/language.*` | Text and speech files of the game's language |
| `src/bild/render/granny_async.*` | Animation update on a worker thread (`[Render] AsyncAnimation`) |
| `src/bild/render/granny_parallel.*` | `[Render] AnimationThreads`: Granny's animation controls sampled on several threads, split by skeleton |
| `src/bild/render/granny_mesh.*` | Granny's meshes, bone bindings and influence lists as its deform routine sees them |
| `src/bild/render/gpu_skin.*` | `[Render] GpuSkinning`: Granny's deform reduced to the bone matrices, characters drawn by the backend's skinning shader |
| `src/bild/diag/skin_check.*` | `[Debug] SkinCheck`: Granny's skinning data read back and checked |
| `src/bild/render/ground_quads.*` | The ground's quad batcher straight to the batcher (`[Render] BatchGround`) |
| `src/bild/render/sprite_quads.*` | The sprite batcher's flushes (water tiles, objects) straight to the batcher (`[Render] BatchSprites`) |
| `src/bild/render/ground_mesh.*` | `[Render] GroundMesh`: the ground's tiles and blend layers in vertex buffers per sector, drawn by the backend's ground shader |
| `src/bild/perf/map_cache.*` | Hash index in front of the map record caches (`[Render] RecordIndex`) |
| `src/bild/perf/fast_math.*` | Faster replacements for the game's x87 math helpers (`[Render] FastMath`) |
| `src/bild/perf/sound_lock.*` | The sound system's kernel mutex as a user-mode lock (`[Render] SoundLock`) |
| `src/bild/diag/d3d_stats.*` | `[Debug] D3DStats` frame statistics |
| `src/bild/perf/file_cache.*` | The pak files and music read once in the background into Windows' file cache (`[Render] WarmFileCache`) |
| `src/bild/diag/file_io_stats.*` | `[Debug] D3DStats`: the game's and Miles' file opens and reads timed, slow ones logged |
| `src/bild/controller/controller.*` | `[Controller]`: the pad driving the game through injected input and its own cursor (walking, attacks, windows) |
| `src/bild/controller/click_walk.*` | Click-to-walk targets beyond the path finders' 64x64 tile grid pulled back into it (large views) |
| `src/bild/controller/hero_move.*` | Controller walking: the game's follow-the-cursor move order sent directly, with the left button's held state it needs |
| `src/bild/controller/aim_assist.*` | Controller targets from the game's list of what can be picked on the screen; the world pick hooked for walking and aiming |
| `src/bild/controller/ui_nav.*` | D-pad navigation: the controls of the open windows, told apart by the exe's RTTI |
| `src/bild/controller/options_screen.*` | The game's Options window as SacredBild's own screen while the controller is in use, with the controller's settings and bindings |
| `src/bild/controller/` | `gamepad.*` (SDL3 on a thread of its own), `inject.*` (keys and mouse buttons handed to the game's window procedure and polls), `input_mode.*` (controller or keyboard and mouse), `bindings.*` (`[Controller.Bindings]`) |
| `src/bild/overlay/`, `src/bild/controller/prompts.*` | Drawn into the back buffer before each present (Direct3D 9 backend): `overlay.*` (Dear ImGui screens), `prompts.*` (controller button prompts, one draw call from the atlas `prompts.png`) |
| `src/bild/diag/world_passes.*` | `[Debug] D3DStats`: the world view's passes timed separately |
| `src/bild/render/batcher.*`, `atlas.*`, `fvf.h` | Draw merging, the texture atlas, vertex format layout |
| `src/bild/net/` | LAN games over VPNs: `lan_client.*` (sacred.exe), `lan_server.*` (gameserver.exe), `lan_protocol.*` (announcements), `adapters.*` (network adapters), `connection.*` (`NoDelay`, one send per message, TCP statistics) |
| `src/bild/net/udp_*`, `tincat_shim.*`, `address.*` | `[Net] Udp`: `udp_endpoint.*` (the IPv4 and IPv6 sockets and their I/O thread), `address.*` (either family), `udp_transport.*` (KCP sessions), `tincat_shim.*` (TinCat's sockets on those sessions), `udp_protocol.*` (wire formats) |
| `src/bild/net/matchmaker.*` | `[Net] Matchmaker` client: publishing (gameserver), listing and joining (sacred.exe) |
| `matchmaker/` | The matchmaking server (Go) |
| `tools/test/nettest/` | Test bench for the UDP connection and the matchmaker without the game: loss, delay, outages, address changes (`-DSACREDBILD_NETTEST=ON`) |

Every header starts with what its module does and why.

## Tools

Python 3 with `pefile` and `capstone`. They work on the English GOG exes, set with the environment variables
`SACRED_ENG` and `GAMESERVER_ENG` (defaults in `tools/sigs/gen_sigs.py`). With `SACRED_DE` and `GAMESERVER_DE` pointing
at the German ones, the generators and `check_hooks.py` check those as well; without them they skip them with a
warning, and the generated files name only the builds they were checked against.

Generated files in the repository, and the checks:

| Tool | Does |
|---|---|
| `tools/sigs/gen_sigs.py` | Writes `src/bild/engine/sacred_sigs.inc`, `gameserver_sigs.inc` and `tools/sigs/data/addresses.json`: a signature for every address, taken from the English build and accepted only if it matches exactly once in it and in the German one |
| `tools/sigs/gen_res_sites.py` | Writes `src/bild/display/resolution_sites.inc`: the 1024x768 constants to patch, each verified and with a signature |
| `tools/sigs/check_hooks.py` | Checks that every hook and typed function (SacredBild's `sacred_addr.h` and Sacred Gear's `gear_addr.h`) declares as many stack arguments as the function pops |
| `tools/re/gen_prompts.py` | Writes `src/bild/controller/prompts.png`: the button prompt atlas, from the [Input Prompts](https://github.com/meritite-union/input-prompts) images (needs Pillow; see the script's header) |
| `tools/re/gen_shaders.py` | Writes `src/bild/ddraw9/skin_shaders.inc`, `ground_shaders.inc` and `smaa_shaders.inc`: `skin.hlsl` and `ground.hlsl` compiled to vs_2_0, `smaa.hlsl` to vs_3_0 / ps_3_0 per preset, with `fxc` from the Windows SDK |
| `tools/sigs/sigs.py`, `funcs.py`, `eng.py` | Shared code: signature search, function boundaries and names (`tools/sigs/data/functions_eng.tsv`, exported from Ghidra), the English exe |

Reverse engineering helpers (how addresses and offsets were found):

| Tool | Does |
|---|---|
| `tools/sigs/eng.py <address> [count]` | Disassembles the English exe |
| `tools/re/rtti.py [regex]` | Class names -> vtables, from the RTTI |
| `tools/re/vt.py <vtable>...` | Vtable slots |
| `tools/re/scan_res.py` | Every instruction that uses a 1024x768-related constant |
| `tools/sigs/check_rets.py <address>...` | `ret` sizes, i.e. stack argument counts |
| `tools/re/profile_report.py <SacredBild-profile.txt>` | Names the hot spots of a `[Debug] Profiler` profile |

## Game-launch lock

Several agents share the game folders. Launches run in parallel (each with its own `-gear-instance=NAME`, see
[Parallel runs](Gear.md#parallel-runs)); only copying DLLs and mods into a game folder collides, so that step takes the
machine-wide lock `tools/game-lock.ps1` (`%ProgramData%\sacred-bild\game.lock`; override with `-LockPath` or
`SACRED_GAME_LOCK`). Hold it only for the copy:

    powershell -NoProfile -File tools\game-lock.ps1 acquire -Id <agent-id>
    ... copy ddraw.dll / SacredGear.dll / mods ...
    powershell -NoProfile -File tools\game-lock.ps1 release -Id <agent-id>
    Sacred.exe -gear-instance=<agent-id> ...

or `game-lock.ps1 run -Id <agent-id> -- <copy command> [args]` (acquires, runs, releases, exits with the command's exit
code). `status` shows the holder; `release` refuses (exit 3) if another id holds it, `-Force` overrides; `acquire`
exits 2 on timeout (`-TimeoutSeconds`, default 3600). A lock is taken over after `-MaxAgeMinutes` (default 30) or when
its owner PID is gone (`run` records its own PID; with `acquire` pass `-OwnerPid`).

## Build

Requires Visual Studio 2026 (MSVC, Win32 toolset), CMake 3.25+ and vcpkg with `VCPKG_ROOT` set
(vcpkg provides [Detours](https://github.com/microsoft/Detours), [gtl](https://github.com/greg7mdp/gtl),
[Dear ImGui](https://github.com/ocornut/imgui), [SDL3](https://www.libsdl.org) and
[KCP](https://github.com/skywind3000/kcp), and for Sacred Gear [RmlUi](https://github.com/mikke89/RmlUi) with FreeType and [stb](https://github.com/nothings/stb)). The matchmaker needs Go 1.27 (`cd matchmaker && go build`).

```sh
cmake --preset msvc-x86
cmake --build --preset release      # -> out/build/msvc-x86/RelWithDebInfo/ddraw.dll
python tools/sigs/check_hooks.py         # checks the hooks against the exe (English, and German if set)
```

The DLL is statically linked against the CRT and imports only Windows system DLLs (`d3d9.dll` is loaded at run time).
vcpkg builds SDL3 with its gamepad support only (`triplets/x86-windows-static.cmake`, an overlay of vcpkg's triplet).
For development, symlink `ddraw.dll`/`ddraw.pdb` in the game folder to the build output instead of installing.
`.github/workflows/` builds every push and pull request; pushes to `main` publish a nightly prerelease, `v*` tags a
release.

## Sacred Gear

Sources are in `src/gear/`. Sacred Gear links the code it shares with SacredBild (log, patches and hooks, signature search)
from the `sacred_core` library. Building: part of the normal build (`out/build/msvc-x86/RelWithDebInfo/SacredGear.dll`;
`-DSACREDGEAR=OFF` leaves it out); vcpkg provides LuaJIT, statically linked. The engine facts it relies on are in
[RE notes](RE_NOTES.md#objects-items-saving-and-the-ui-pass-sacred-gear).

| Path | What it is |
|---|---|
| `src/gear/main.cpp` | DLL entry: one instance per process, signatures, hooks; headless in `gameserver.exe` (file overlay only) |
| `src/gear/lua_host.*`, `lua_native.h`, `*_lua.cpp` | The LuaJIT state on the engine thread, the time budget and the kernel calls; the native functions for the kernel, each area's in a `*_lua.cpp` next to its module (`world/world_lua.cpp`, `items/items_lua.cpp`, `ui/ui_lua.cpp`, `save_lua.cpp` ...), the helpers they share in `lua_native.h` |
| `src/gear/lua/kernel*.lua`, `kernel.parts` | The trusted Lua side, in parts by area joined in the order of `kernel.parts` into one chunk (one scope) at build time and embedded as a resource: manifests, load order, sandboxes, events, table views, the mod catalog (settings, key binds, live load and unload, `gear_mods.json`) |
| `src/gear/lua/modmanager.lua` | The Mods screen, loaded as the mod `gear-mods`; the only mod that gets the kernel's `admin` table |
| `src/gear/lua/*.lua` | Libraries shipped in the DLL (`fmt`, `lookup`, `store`, `toast`, ...) |
| `src/gear/hooks.*` | The engine hooks: the per-frame tick (the game's per-frame zoom function: anything replacing that call must still let it run) |
| `src/gear/world/world.*`, `party.*`, `inventory.*` | Objects through the game's object manager; companions; containers and item export |
| `src/gear/ui/ui.*`, `itemicon.*`, `replace.*` | Mod drawing (draw commands, the hooks in the UI pass, Direct3D 7 quads with the game's states saved and restored, fonts and UI images, window and UI state queries); item icons; replaced windows |
| `src/gear/items/pickup.*`, `drops.*` | Pickup rules; drop hooks ([Gear drops](GearDrops.md)) |
| `src/gear/overlay.*` | The file overlay and delta patches |
| `src/gear/ui/rml.*` | RmlUi for `gear.ui.rml`: its system, file and render interfaces (meshes recorded as draw commands, `Kind::Mesh` in `ui.cpp`), a context per document, element access and the event queue for Lua. Documents read only the mod's own folder. Clean documents are not updated or rendered again and are drawn from retained vertex buffers (`Kind::Retained`, [RmlRetained.md](RmlRetained.md)) |
| `src/gear/netmsg.*`, `src/gear/realm.*` | Custom net messages; the lobby host and port of the network menu |
| `src/gear/chat.*`, `input.*`, `padnav.*`, `itemtip.*`, `text.*`, `gamecfg.*`, `sitehook.*` | Chat lines; remapping the game's keys; the controller on mod UI; item tooltips; game text; `Settings.cfg`; engine hook sites (`gear.hooks`) |
| `src/gear/save.*`, `savegames.*` | Saving and loading; the savegame list and per-savegame mod data |
| `src/gear/ui/keyblock.*`, `src/gear/pipe.*` | The keyboard kept from the game while a screen captures it; named pipe clients |
| `src/gear/automation.*` | Automation: options, input isolation, UI state, savegame loading, screenshots |
| `src/gear/tables.*` | The engine's data tables, read in place with bounds and fault checks |
| `src/gear/engine/gear_addr.h`, `gear_sigs.inc` | The sacred.exe addresses Sacred Gear uses; their signatures (generated by `tools/sigs/gen_gear_sigs.py`) |
| `src/gear/meta/*.lua` | The LuaLS annotations of the API: the source of `docs/GearApi.md` and of the MDK |
| `src/gear/mdk/` | The MDK's own files (README, template, tools); the kit itself is generated (`tools/gearapi/gen_gear_mdk.py`) and published by CI, not committed |
| `src/gear/examples/`, `src/gear/tests/kernel_test.lua` | Example mods and automation tests; the kernel's tests with the native side mocked (`luajit src/gear/tests/kernel_test.lua`) |

**With SacredBild.** `ddraw.dll` exports `SacredBild_UiPushFrame(window, confine)`, `SacredBild_UiPopFrame()`,
`SacredBild_UiScreenBounds(float[4])`, `SacredBild_UiSetWindowFrame(slot, x, y, set)`,
`SacredBild_UiGetWindowFrame(slot, float*, float*)`, `SacredBild_UiPlacement(float[3])` (screen pixels per layout unit
and the frame's screen origin: vector text is rasterized for it, rectangles and images snap to its pixels),
`SacredBild_UiGetWindowBounds(slot, float[4])` (where a window drew in the last frame) and
`SacredBild_UiSetCursorFilter(fn(x, y))` (Gear tells where mod UI lies; SacredBild hides the cursor from the game's UI
reads there and blocks the world pick). Gear draws a command with `window` in that window's UI frame and the others in
the unconfined canvas; game windows are moved by giving them a UI frame of their own. The mouse wheel over mod UI comes
through SacredBild's window procedure (also the controller's right stick), without SacredBild through a message hook
on the game window's thread. Sacred Gear exports `SacredGear_IsolatedForeground()` for isolated automation runs.
Without SacredBild the layout is drawn as the game draws it and game windows stay where they are.

### The Lua API annotations

The annotations in `src/gear/meta/*.lua` are part of the API: a change to what the Lua side offers is not done until
they say so.

1. Change the code (the kernel part of the area, `src/gear/lua/kernel_<area>.lua`, and the native tables in the area's `*_lua.cpp`).
2. Change the matching annotation: a new function gets a description, `---@param` and `---@return` lines; a new
   field a `---@field`; a new option goes into the options class of the function; a new event gets a `gear.on`
   overload and a `GearEventName` entry.
3. Run `python tools/gearapi/gen_gear_api_docs.py` (rewrites `docs/GearApi.md`; never edit that file) and `python
   tools/gearapi/check_gear_api.py`. The MDK is built from the same sources by CI (`python tools/gearapi/gen_gear_mdk.py` builds it locally
   in `out/mdk`).

`check_gear_api.py` reads the kernel's parts and `lua_host.cpp` with every `*_lua.cpp` as text and compares names both ways (functions, fields,
events, options, manifest keys); types and signatures are reviewed by hand. It also fails if `docs/GearApi.md` is not
what the generator produces. If it cannot find a place in the code it expects, the code changed shape: update the check
rather than silence it. CI runs it, with the kernel tests, in its text-checks job.

The format:

- One file per area; the list is `FILES` in `tools/gearapi/gear_meta.py`. Every file starts with `---@meta _`.
- `-- ## Title` starts a section of the reference; `--: text` lines right under it are the section's introduction (plain
  comments to LuaLS). Concepts that hold for a whole area go there, not into `docs/Gear.md`.
- A namespace or object type is a `---@class` followed by a local table (`local GearWorld = {}`); its functions are
  `function GearWorld.hero() end` (static) or `function Creature:hp() end` (method). The `gear` and `os` globals are
  `gear = {}` and `os = {}` after their class.
- A plain comment `-- path gear.world` right above the class's doc block says how it is reached; the reference prints
  signatures with it (`gear.world.hero()`, `creature:hp()`).
- Options tables are classes whose name ends in `Opts` (`GearTextOpts`); every `opts.x` the kernel reads must be a field
  of one, and `GearPickupRules` holds the keys of `pickup_rules`. Enumerated strings are `---@alias` with
  `---| 'value' # meaning`.
- Return a value or `nil` and a message as `---@return Creature? creature` and `---@return string? error`.

To check the examples with LuaLS: `lua-language-server --check src/gear/examples --configpath .luarc.json` from the
repository root (not part of the build).

### Tools

Python 3 with `pefile` and `capstone`, `SACRED_ENG` as for SacredBild's tools:

| Tool | Does |
|---|---|
| `tools/sigs/gen_gear_sigs.py` | Writes `src/gear/engine/gear_sigs.inc` from the entries listed in it |
| `tools/sigs/check_gear_overlap.py` | Fails if Sacred Gear and SacredBild patch the same bytes (two Detours hooks of one function are fine: they chain). Signatures are searched in the exe file's code, so a patch of one never hides the bytes the other's signatures read |
| `tools/gearapi/check_gear_api.py` | Fails if the annotations and the Lua API disagree, or `docs/GearApi.md` is out of date |
| `tools/gearapi/gen_gear_api_docs.py` | Writes `docs/GearApi.md` from `src/gear/meta/*.lua` and `docs/GearApi.head.md` |
| `tools/gearapi/gen_gear_mdk.py` | Writes the MDK to `out/mdk/sacred-gear-mdk` (`--zip`: also the zip); CI runs it and publishes the kit |

