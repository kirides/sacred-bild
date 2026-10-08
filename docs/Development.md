# Development

SacredBild is one DLL, `ddraw.dll`, which the game loads from its folder in place of Windows' own. Start in
`src/main.cpp`: `DllMain` reads `SacredBild.ini` (`src/config.*`); with the settings window, the rest waits for the
exe's entry point, after the window (`src/settings_window.*`). Then it loads the DirectDraw implementation the game
will get (`src/proxy.*`), finds the game's code by byte signature (`Sacred::resolveAddresses`) and installs the
patches (`Sacred::installHooks` in `src/game/build.cpp`, which lists every module in order). Each module patches
the game in its `install()` with [Detours](https://github.com/microsoft/Detours) hooks or checked byte patches
(`src/patch.*`: a write happens only if the bytes there are the expected ones).

What the game's code at those addresses does is in [`RE_NOTES.md`](RE_NOTES.md) and in the comments of
`src/game/sacred_addr.h`; the addresses refer to the English GOG build (Ghidra program `/Sacred.exe (GOG)`).

## Source files

| Path | What it is |
|---|---|
| `src/main.cpp` | DLL entry: game (`sacred.exe`) or LAN gameserver (`gameserver.exe`) setup |
| `src/proxy.*`, `src/exports.def` | The `ddraw.dll` exports, to the Direct3D 9 backend or the chain-loaded ddraw |
| `src/system_ddraw.*` | `Backend=chain` on Windows' own ddraw: render targets over 2048 pixels |
| `src/config.*`, `src/config/` | `SacredBild.ini`: loading it, and the settings in a header per ini section (`Config::render`, `Config::net`, ...) |
| `src/settings_window.*` | The settings window before the game starts (`[Launcher] HideSettingsWindow`): what it shows, declared control by control |
| `src/ui/form.*` | `Ui::Form`: settings windows declared in code (pages, groups, rows of controls bound to ini keys), laid out and run as dialogs |
| `src/log.*`, `src/crash_dump.*` | `SacredBild.log`, minidumps on crashes |
| `src/patch.*`, `src/sig.*` | Code patches and hooks; byte signature search |
| `src/spin_lock.h` | Locks for the render thread's hot paths |
| `src/mem.h` | Fields of the game's objects by byte offset (`Mem::member`, `Mem::field`) |
| `src/profiler.*` | `[Debug] Profiler` |
| `src/ddraw9/` | The Direct3D 9Ex backend: `directdraw.*` (IDirectDraw7, IDirect3D7, the exports), `surface.*` (IDirectDrawSurface7), `device.*` (IDirect3DDevice7), `vertex_buffer.*`, `device_skin.cpp` / `device_ground.cpp` (the skinning and ground shaders, `skin.hlsl`, `ground.hlsl`), `gpu.*` (the Direct3D 9 device, presentation), `format.*` (pixel formats); `d3d9_api.h` puts Direct3D 9 into namespace `d9`, its headers clash with Direct3D 7's |
| `src/game/build.*` | Which exe this is, address lookup, the list of game modules |
| `src/game/sacred_addr.h`, `sacred_sigs.inc` | The sacred.exe addresses SacredBild uses, functions with their signatures (`Sacred::Thiscall`, `Cdecl`; `tools/check_hooks.py` checks them), and the struct offsets not yet in `src/sacred/`; their signatures (generated) |
| `src/sacred/` | The game's classes as structs: fields at their offsets and vtables as structs of function pointers, both checked by `static_assert`, member functions calling the game's code (`cEngine`, `cObject` / `cCreature`, `cObjectManager`, `cMouse`, `cWorldView`, ...); `address.h` the typed addresses |
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
| `src/game/granny_parallel.*` | `[Render] AnimationThreads`: Granny's animation controls sampled on several threads, split by skeleton |
| `src/game/granny_mesh.*` | Granny's meshes, bone bindings and influence lists as its deform routine sees them |
| `src/game/gpu_skin.*` | `[Render] GpuSkinning`: Granny's deform reduced to the bone matrices, characters drawn by the backend's skinning shader |
| `src/game/skin_check.*` | `[Debug] SkinCheck`: Granny's skinning data read back and checked |
| `src/game/ground_quads.*` | The ground's quad batcher straight to the batcher (`[Render] BatchGround`) |
| `src/game/ground_mesh.*` | `[Render] GroundMesh`: the ground's tiles and blend layers in vertex buffers per sector, drawn by the backend's ground shader |
| `src/game/map_cache.*` | Hash index in front of the map record caches (`[Render] RecordIndex`) |
| `src/game/d3d_stats.*` | `[Debug] D3DStats` frame statistics |
| `src/game/controller.*` | `[Controller]`: the pad driving the game through injected input and its own cursor (walking, attacks, windows) |
| `src/game/hero_move.*` | Controller walking: the game's follow-the-cursor move order sent directly, with the left button's held state it needs |
| `src/game/aim_assist.*` | Controller targets from the game's list of what can be picked on the screen; the world pick hooked for walking and aiming |
| `src/game/ui_nav.*` | D-pad navigation: the controls of the open windows, told apart by the exe's RTTI |
| `src/game/options_screen.*` | The game's Options window as SacredBild's own screen while the controller is in use, with the controller's settings and bindings |
| `src/input/` | `gamepad.*` (SDL3 on a thread of its own), `inject.*` (keys and mouse buttons handed to the game's window procedure and polls), `input_mode.*` (controller or keyboard and mouse), `bindings.*` (`[Controller.Bindings]`) |
| `src/overlay/` | Drawn into the back buffer before each present (Direct3D 9 backend): `overlay.*` (Dear ImGui screens), `prompts.*` (controller button prompts, one draw call from the atlas `prompts.png`) |
| `src/game/world_passes.*` | `[Debug] D3DStats`: the world view's passes timed separately |
| `src/render/batcher.*`, `atlas.*`, `fvf.h` | Draw merging, the texture atlas, vertex format layout |
| `src/net/` | LAN games over VPNs: `lan_client.*` (sacred.exe), `lan_server.*` (gameserver.exe), `lan_protocol.*` (announcements), `adapters.*` (network adapters), `connection.*` (`NoDelay`, one send per message, TCP statistics) |
| `src/net/udp_*`, `tincat_shim.*`, `address.*` | `[Net] Udp`: `udp_endpoint.*` (the IPv4 and IPv6 sockets and their I/O thread), `address.*` (either family), `udp_transport.*` (KCP sessions), `tincat_shim.*` (TinCat's sockets on those sessions), `udp_protocol.*` (wire formats) |
| `src/net/matchmaker.*` | `[Net] Matchmaker` client: publishing (gameserver), listing and joining (sacred.exe) |
| `matchmaker/` | The matchmaking server (Go) |
| `tools/nettest/` | Test bench for the UDP connection and the matchmaker without the game: loss, delay, outages, address changes (`-DSACREDBILD_NETTEST=ON`) |

Every header starts with what its module does and why.

## Tools

Python 3 with `pefile` and `capstone`. They work on the English GOG exes, set with the environment variables
`SACRED_ENG` and `GAMESERVER_ENG` (defaults in `tools/gen_sigs.py`). With `SACRED_DE` and `GAMESERVER_DE` pointing
at the German ones, the generators and `check_hooks.py` check those as well; without them they skip them with a
warning, and the generated files name only the builds they were checked against.

Generated files in the repository, and the checks:

| Tool | Does |
|---|---|
| `tools/gen_sigs.py` | Writes `src/game/sacred_sigs.inc`, `gameserver_sigs.inc` and `tools/data/addresses.json`: a signature for every address, taken from the English build and accepted only if it matches exactly once in it and in the German one |
| `tools/gen_res_sites.py` | Writes `src/game/resolution_sites.inc`: the 1024x768 constants to patch, each verified and with a signature |
| `tools/check_hooks.py` | Checks that every hook declares as many stack arguments as the hooked function pops |
| `tools/gen_prompts.py` | Writes `src/overlay/prompts.png`: the button prompt atlas, from the [Input Prompts](https://github.com/meritite-union/input-prompts) images (needs Pillow; see the script's header) |
| `tools/gen_shaders.py` | Writes `src/ddraw9/skin_shaders.inc` and `ground_shaders.inc`: `skin.hlsl` and `ground.hlsl` compiled to vs_2_0 with `fxc` from the Windows SDK |
| `tools/sigs.py`, `funcs.py`, `eng.py` | Shared code: signature search, function boundaries and names (`tools/data/functions_eng.tsv`, exported from Ghidra), the English exe |

Reverse engineering helpers (how addresses and offsets were found):

| Tool | Does |
|---|---|
| `tools/eng.py <address> [count]` | Disassembles the English exe |
| `tools/rtti.py [regex]` | Class names -> vtables, from the RTTI |
| `tools/vt.py <vtable>...` | Vtable slots |
| `tools/scan_res.py` | Every instruction that uses a 1024x768-related constant |
| `tools/check_rets.py <address>...` | `ret` sizes, i.e. stack argument counts |
| `tools/profile_report.py <SacredBild-profile.txt>` | Names the hot spots of a `[Debug] Profiler` profile |

## Build

Requires Visual Studio 2026 (MSVC, Win32 toolset), CMake 3.25+ and vcpkg with `VCPKG_ROOT` set
(vcpkg provides [Detours](https://github.com/microsoft/Detours), [gtl](https://github.com/greg7mdp/gtl),
[Dear ImGui](https://github.com/ocornut/imgui), [SDL3](https://www.libsdl.org) and
[KCP](https://github.com/skywind3000/kcp)). The matchmaker needs Go 1.27 (`cd matchmaker && go build`).

```sh
cmake --preset msvc-x86
cmake --build --preset release      # -> out/build/msvc-x86/RelWithDebInfo/ddraw.dll
python tools/check_hooks.py         # checks the hooks against the exe (English, and German if set)
```

The DLL is statically linked against the CRT and imports only Windows system DLLs (`d3d9.dll` is loaded at run time).
vcpkg builds SDL3 with its gamepad support only (`triplets/x86-windows-static.cmake`, an overlay of vcpkg's triplet).
For development, symlink `ddraw.dll`/`ddraw.pdb` in the game folder to the build output instead of installing.
`.github/workflows/` builds every push and pull request; pushes to `main` publish a nightly prerelease, `v*` tags a
release.
