# Rendering

How SacredBild draws the game, and what it measures. The settings named here are in
[`SacredBild.ini`](Configuration.md).

## Direct3D 9Ex backend

`src/ddraw9/`: DirectDraw 7 and Direct3D 7 implemented on Direct3D 9Ex, for the subset Sacred uses (windowed swap
chain, one render target with z-buffer, managed textures, system memory surfaces for GDI text, the fixed-function
device). Both APIs are fixed-function with the same vertex formats and render states, so device calls translate
almost one to one. Frames are presented with the flip model (the blit model with `VSync=0`, which is not held to
the refresh rate; `MaxFrameLatency`), and the per-call layers of Windows' Direct3D 7 runtime and DDrawCompat are
gone. Anything the backend doesn't implement is logged once (`Direct3D 9 backend: not supported: ...`).

The backend runs on any `d3d9.dll`: `[DDraw] D3D9`, else a `d3d9.dll` next to the game exe (e.g.
[DXVK](https://github.com/doitsujin/dxvk)'s 32-bit one), else Windows' own. `SacredBild.log` names the one in use
(`Direct3D 9: using ...`).

With `[DDraw] Backend=chain`, SacredBild loads [DDrawCompat](https://github.com/narzoul/DDrawCompat) behind itself
(`SacredBild\DDrawCompat.dll`), or Windows' own `ddraw.dll` without it (whose Direct3D 7 runtime refuses render
targets over 2048 pixels; SacredBild works around that). SacredBild's own screens (the controller's Options, button
prompts) need the Direct3D 9 backend.

## Movies through Media Foundation

`[DDraw] MediaFoundation`, always with the Direct3D 9 backend: the game decodes its WMV movies with DirectShow into
DirectDraw surfaces, which needs a real DirectDraw, and its loop stops processing window messages. SacredBild plays
them with the Media Engine instead (audio included), draws them over the whole screen with their aspect ratio kept,
and keeps the window responsive; ESC, space and mouse buttons skip as before. Where the Media Engine is missing
(Windows 7, N editions) or can't play a movie (Wine), they are decoded with the game's DirectShow streams into a
system memory surface of Windows' own DirectDraw instead, and drawn the same way (`[Debug] MovieFallback`).

## Batched world rendering

The world view issues thousands of small sprite and ground draws per frame (almost 10,000 zoomed out at 1920x1200).
SacredBild records state changes instead of applying them and merges consecutive draws that end up with the same
state into one call; small textures are copied into shared atlas pages so draws with different textures merge as
well (`[Render] Batch*`, `Atlas*`).

## Ground kept on the GPU

Every frame the game rebuilds a quad for each visible ground tile and each of its blend layers (2560x1440 zoomed
out: ~4,000 tiles and ~7,000 layers, about a third of the world view's time) and draws them through the batcher.
A tile's quad only depends on the tile and on the camera, which moves the whole ground at once. With
`[Render] GroundMesh`, each loaded sector keeps its tiles in vertex buffers, filled once as tiles come into view, and
a vertex shader places them on the screen where the game would have drawn them: a few draws per sector and pass. The
ground textures are copied into atlas pages of their own. Tiles that change are noticed when they are walked over,
and their sector is built again. The water and lava tiles, objects and characters are drawn as before (Direct3D 9
backend only; idea from the [SacredEngineRemake](https://github.com/Aytackydln/SacredEngineRemake), which composes
each sector's terrain once).

## Characters animated on the GPU

Granny skinned every character and its shadow on the CPU each frame (a fifth of the render thread zoomed out). With
the Direct3D 9 backend, a vertex shader does that from meshes kept on the GPU and lights them as Direct3D 7 did
(2560x1440 zoomed out: 94 -> 132 fps; `[Render] GpuSkinning`, `OffscreenPoses`). The animation update runs on a
worker thread (`AsyncAnimation`); with `AnimationThreads` its biggest part, sampling every character's animations,
is split by character over several threads (2560x1440 zoomed out: walk 2.1 -> 0.6 ms, ~150 -> ~200 fps;
`[Debug] AnimationCheck` compares it with one thread).

## Diagnostics

Optional per-second frame stats (`[Debug] D3DStats`: draw calls, texture switches, unique textures, time spent in
the world renderer, UI, flip and inside Direct3D, what ended each batch, texture memory) and an optional sampling
profiler (`[Debug] Profiler`).

When the game or its gameserver crashes, a minidump goes next to the exe (`SacredBild-crash-*.dmp`,
`SacredBild-server-crash-*.dmp`; `[Debug] CrashDump`) and the log names the exception and where it happened.
