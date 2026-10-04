# Sacred Gold (DE build) renderer notes

Target: German `sacred.exe` 2.0, PE timestamp `0x451BBE74`, image base `0x400000`.
Ghidra program: `/sacred.exe (DE)` (functions below are named there). An English build
(`/Sacred.exe (GOG)`, timestamp `0x452F85C7`) is in the same project; around the renderer its addresses are
ENG + `0x130`, elsewhere the offset differs. All addresses below are DE. SacredBild supports both through byte
signatures (`tools/gen_sigs.py`, `tools/data/addresses.json` lists every patched address in both builds).

## Big picture

Sacred is *not* a CPU blitter at its core: it renders with **Direct3D 7 (TnL HAL)** through a thin
wrapper, `dxDriver7`. The isometric world is real 3D geometry under an orthographic projection; sprites
and UI are pretransformed (`XYZRHW|DIFFUSE|SPECULAR|TEX1`, FVF `0x1C4`) quads. The CPU cost comes from
issuing thousands of small draws: a measured in-game frame at 2560x1440 (zoom ~1) did about 6,100
`SetTexture` calls with 4,070 actual texture switches and spent ~10.3 ms in the world renderer. Zooming
out to 2.0 shows four times the area and multiplies that.

Two threads present frames:
- `cUI_Manager_runThread` (`0x756940`): menus. Clears target+Z, renders UI, software cursor, flips.
  Sleeps 20 ms per frame in menus (~46 fps).
- `cEngine_renderThreadRun` (`0x60E3F0`): in-game. Per frame: clear Z, `beginScene`, world view
  (`vtable[5]`, `cWorldView0_render`), label layout, overlays, UI (`cUI_Manager_render`), `endScene`, flip.

## dxDriver7 (global `g_pDxDriver` = `0xCDA99C`)

| Offset | Field |
|---|---|
| +0x14..+0x94 | copy of the display mode record (DDSURFACEDESC2): `+0x1C` height (u16), `+0x20` width (u16), `+0x24` pitch |
| +0x90 | 1 = fullscreen |
| +0x94 | bits per pixel |
| +0xB0 | HWND |
| +0xB4 / +0xB8 / +0xBC | IDirectDraw7 / primary / back buffer (3D render target) |
| +0xC8 / +0xCC | IDirect3D7 / IDirect3DDevice7 (SacredBild swaps in `DeviceProxy`) |
| +0x1DC / +0x1EC | flip source / destination rects (windowed) |

| Address | Function |
|---|---|
| `0x815620` | `initApp`: `cDxDevices_findMode(1024, 768, bpp, flags)` at `0x815C82`, then `dxDriver7_ctor` |
| `0x644260` | `cDxDevices_findMode` (4 args) returns a 0x84-byte mode record |
| `0x644960` | `dxDriver7_ctor` |
| `0x645390` | `dxDriver7_init` (3 args): DirectDrawCreateEx, cooperative level, surfaces, z-buffer, CreateDevice |
| `0x644BE0` | `dxDriver7_createSurface` |
| `0x645F70` | `dxDriver7_flip`: windowed = `primary->Blt(back)`; also a CPU fade effect |
| `0x646280` / `0x6462F0` | `dxDriver7_lockBack` / `unlockBack` |
| `0x6466B0` | `dxDriver7_drawTexturedQuad`: one `DrawPrimitive(TRIANGLESTRIP, 0x1C4, 4)` per sprite |
| `0x6469D0` | `dxDriver7_drawLoadingScreen`: GDI on the back buffer DC, 1024x768 layout |
| `0x646F50` / `0x646FD0` | `beginScene` / `endScene` (ref-counted) |

Main window: `createMainWindow` (`0x664910`), `CreateWindowExA` with `WS_OVERLAPPEDWINDOW|WS_VISIBLE`
(return address `0x664A5E`).

## What the game needs from DirectDraw (SacredBild's Direct3D 9 backend, `src/ddraw9/`)

- Imports `DirectDrawCreateEx`, `DirectDrawCreate`, `DirectDrawEnumerateExA`. Interfaces it asks for: IDirectDraw7,
  IDirect3D7, the HAL / T&L HAL device GUIDs, and IDirectDraw + IDirectDrawSurface (version 1) for the video stream
  only. No gamma control, no other interface versions.
- Startup enumeration (`0x644390`): DirectDrawEnumerateExA, then per device DirectDrawCreateEx, QI IDirect3D7,
  GetCaps (DDCAPS size 0x17C), EnumDisplayModes, EnumDevices.
- `dxDriver7_init`, windowed: SetCooperativeLevel(hwnd, NORMAL | MULTITHREADED | FPUSETUP); primary
  (PRIMARYSURFACE | 3DDEVICE | VIDEOMEMORY, color-filled); back buffer `dxDriver7_createSurface(0, w, h, 0xd)` =
  3DDEVICE | VIDEOMEMORY, no pixel format; EnumZBufferFormats for 32 bits with 8 stencil bits (16 bits at 16 bpp,
  callback `0x644F90` matches flags, depth and stencil depth); the z-buffer is AddAttachedSurface'd to the back
  buffer; CreateDevice with the back buffer (three tries), then the primary; a clipper on the primary; Lock/Unlock of
  primary and back buffer only to read pitch and format. Fullscreen: SetDisplayMode, flip chain, Flip.
- Frames: windowed `primary->Blt(client rect in screen coordinates, back buffer, rect, DDBLT_WAIT)`.
  `IDirectDraw7::WaitForVerticalBlank` runs before every `dxDriver7_lockBack` and `+0x290` times per flip.
- `dxDriver7_createSurface` types: 1 = texture (TEXTURE + DDSCAPS2_TEXTUREMANAGE, pixel format from
  EnumTextureFormats; the callback `0x647030` keeps the last format matching bit count and alpha mask); 0 with flag 2 =
  OFFSCREENPLAIN | SYSTEMMEMORY (fonts: GetDC/ReleaseDC for GDI text, Lock, then copied into textures); 3 = windowed
  primary + back buffer; 2 = fullscreen flip chain. GDI also draws the loading screen into the back buffer's DC.
- Granny textures: `0x401BE0` enumerates texture formats (callback `0x403F60` takes 4444, 565, 555, 1555, 888,
  8888, no FourCC) for `GrannyAllowTextureFormat`.
- Movies (`movie\*.wmv`, `0x6A14A0` picks them by id: ASCARON/PUBLISHER/OEM at startup from `initApp`, INTRO,
  EXTRO, act1-8, ...): `openMovieStream` (`0x6A0B40`, cdecl (path, IDirectDraw*, IAMMultiMediaStream** out)) creates
  amstream, adds the DirectDraw object as the primary video stream and the default audio renderer, opens the file.
  `playVideo` (`0x6A0C60`, thiscall on the movie player: `+4` skip count, `+8` HWND; args IDirectDraw*, primary
  IDirectDrawSurface*, stream, 640, 480; returns -1 when skipped). With `COMPAT_VIDEO` it creates a 1024x512 texture
  (caps TEXTURE only), hands it to `IDirectDrawMediaStream::CreateSample` as IDirectDrawSurface and draws it as a TL
  quad after each `IDirectDrawStreamSample::Update`; without, amstream creates its own surface and the game Blts it
  to the primary. Its loop only peeks keyboard messages (removing them) and skips on ESC, space or a mouse button
  (`GetAsyncKeyState`). amstream doesn't work on SacredBild's emulated DirectDraw (no picture, then the window hung
  after skipping), so with the Direct3D 9 backend `openMovieStream` only records the file and `playVideo` plays it
  through Media Foundation (`src/game/movie.cpp`).
- `dxDriver7_beginScene` (`0x646F50`, ref-counted at `+0x1D8`) retries `IDirect3DDevice7::BeginScene` until it
  succeeds: a device whose BeginScene fails hangs the calling thread.

## World view (`cWorldView`, vtables `0x89009C`, `cWorldView0` `0x8900F8`)

- `+0xB60` float **zoom**, clamped to [`g_flZoomMin` 0.5, `g_flZoomMax` 2.0].
- `cWorldView_setZoom` (`0x628210`), `cWorldView_addZoom` (`0x6283F0`), `cWorldView_applyProjection`
  (`0x624F10`), vtable[7] `0x6289D0`: build `matrixOrtho(±267·zoom, ±200·zoom, -600, 2500)` from doubles
  `g_dOrthoLeft..Top` (`0x88EE40..0x88EE58`) and set it as the projection, then store an unzoomed copy
  in `g_unzoomedProjection` (`0x182CCF0`) using push immediates ±267/±200.
  534x400 world units map to 1024x768 px.
- `g_unzoomedProjection` is not a render matrix: `FUN_00623a20` (pixels -> world, `x * (2/P[0]) / 1024`),
  `FUN_00623c40` (world -> pixels, `* 1024 / (2/P[0])`) and their wrappers (`0x6239F0`, `0x623AA0`
  iso -> world, `0x623B60`, `0x623BB0`) use it to convert between world units and pixel offsets. Game logic
  (creature AI, spells, scripts, model placement) calls these everywhere, so the matrix and the 1024/768
  in them must stay unpatched: widening it scaled every 3D model's world position by W/1024, which made
  all characters vanish and broke world clicks and the intro script. Only the device projections set by
  `cEngine_ctor` and `0x60D7E0` (push ±267/±200, `SetTransform(PROJECTION)`) are widened.
- Object/creature passes (`0x62E580`, `0x6300D0`) cull screen positions against `0x4C8` / `0x3C8`
  (1024 + 200, 768 + 200), patched to W + 200 / H + 200. Characters are Granny 3D models
  (`cGranny_render*` around `0x402000-0x410000`), placed in world space; ground and objects are TL sprites.
- `cWorldView_updateViewMetrics` (`0x624AE0`): view size `+0x96AB0 = zoom·1024`, `+0x96AB4 = zoom·768`,
  tile columns `w/96+12`, rows `h/48+24`.
- `cWorldView0_render` (`0x6322B0`, arg: device): walks tile rows (`cWorldView_renderTileRow`
  `0x62B000`), collects objects into growable vectors, then draws them (`0x629420`, `0x62D530`,
  `0x62E580`, `0x62DE70`, `0x6300D0`). Those passes cull projected positions against
  `0 < x < 1024.0f` / `0 < y < 768.0f` (`g_flScreenWidth1024` `0x88E6F4`, `g_flScreenHeight768`
  `0x890040`).
- Ground tiles: `cTileRenderer_instance` (`0x6200A0`, object `g_tileRenderer` `0xAD56C0`).
  `cTileRenderer_addQuad` (`0x61E810`) appends 4 vertices (FVF `0x244`) to an 8192-vertex VB grouped by
  texture; `cTileRenderer_flush` (`0x61FAC0`) draws one `DrawIndexedPrimitiveVB` per texture group.
  This part is already batched.
- `cWorldView_drawOverheadLabels` (`0x627260`): ALT labels; layout area from `cLabelLayout_init`
  (`0x662300`, called with 1024, 768).
- `renderSavePortrait` (`0x4B1740`, thiscall on `0xAAAF00`, args path/304/360/0.6f; called from a UI
  window via `0x5503C0`): clears the back buffer, renders the hero with the same projection constants
  into its center, reads back 304x360 and writes the savegame JPEG (ijl). Not visible on screen.
- View space and screen center: `cWorldView0_render` positions rows relative to `camera - viewSize/2`
  (view size = zoom·1024 x zoom·768, patched to zoom·W x zoom·H), so view space is centered on the screen
  center. The passes then map view to screen as `ftol((v - 512.0f) * zoom) + 512` (and `384`), and cull
  with the inverse `(p - 512.0f) / zoom + 512.0f` against `0..1024` / `-100..818` (tile rows) or
  `-100..888` (`0x62D530`). Float and int halves must change together: at zoom 1 the original pairs cancel
  out, patching only the int half shifts everything by `W/2 - 512`. SacredBild patches all of them
  (`resolution_sites.inc`: `ImmHalfW/H`, `MemHalfW/H`, `MemCullH/H2`), including "center - camera" loads
  (`mov reg, 0x200`) in the object passes and the effect code at `0x41B150`, `0x41B1E0`, `0x41EAF0`.
- Draw lists: `cWorldView_renderTileRow` stores object positions relative to the camera with the center
  added (`iso - cam + 0x200`, and for static objects `iso - (cam - 0x200)` via `add reg, -0x200` at
  `0x62BABB`); the object passes convert them back with `(p - 512.0) / zoom + 512.0`. Every one of these
  centers is patched together (missing the `-0x200` form shifted all buildings by `(512 - W/2) / zoom`).
  Most 2D sprites draw with Z off, so their layering against the 3D characters is draw order. Objects with
  flag `0x200` (buildings, portals: the occluders characters walk behind) draw with Z test and Z write on, with a
  depth computed by hand (see "Depth range" below).
- Camera and depth range: view = look-at from eye `0x182CDA0` (0, 1200, 600) to target `0x182CD80` (0, 0, 0),
  up +Z (set up by static initializers at `0x810DB0`/`0x810DD0`; `0x6285C0` shifts both by the camera). The
  camera looks down at 26.57° from 1341.6 units, so ground `v` world units above the screen center lies at depth
  `1341.6 + 2v`, a point `h` above the ground `2.236h` closer. The world projection's near/far are -600 / 2500
  (`push 0xC4160000` / `0x451C4000` in `0x624F10`, `0x628210`, `0x6283F0`, `0x6289D0`; `cEngine_ctor` and
  `0x60D800` set an initial -1000 / 1500 one that the world view replaces every frame). That covers a 768 px
  view up to zoom 2 (depths 542..2142). At 2560x1440, zoom 2, ground in the top ~164 px lies past the far plane:
  3D models there lost their lower parts (the far side) or vanished. `0x628980` (thiscall (screen y, height))
  gives the Z-tested sprites their depth with the same constants by hand:
  `(2 * (200 - y * 400/768) * zoom - h / sin(26.57°) + view[+0x970AC]) / 3100 + 0.002`, where 200 is the 384 px
  center in world units and `+0x970AC` = camera distance + 600 (`cWorldView_updateViewMetrics`). With the screen
  center at H/2 that put the sprites ~350·zoom units in front of 3D models at the same spot. SacredBild scales the
  range around the camera distance with `H/768` (1440: -2299..3514) and patches the sprite constants to match
  (center `200·H/768`, `-near`, `1/(far - near)`, bias `0.002·768/H`). The model visibility test `0x405810`
  only checks x/y.
- `cWorldView_screenToWorld` (`0x62A0A0`, vtable slot 6): `(mouse - backbuffer/2) * zoom + camera`, uses
  the real back-buffer size.
- Ground layers: `renderTileRow` draws each tile's base through the quad batcher (`+0x86890`, flush
  `0x629420`) and copies tiles with blend layers into an array at `+0xB64` (0x90 bytes each, list heads at
  `+0x3E3C4`, count `+0x3FF1C`, capped at `0x6D5`); `cWorldView_drawTileLayers` (`0x62D530`) draws them after
  all rows. A zoomed-out high-resolution view overflows the cap (later rows lose their blends: staircase
  edges), so SacredBild flushes batcher + layers between rows when the array is nearly full.
- Row walk: only the 3x3 sectors around the camera are loaded (64x64 tiles each; `view+0x970B0` 9 sector
  pointers, tile table at `+0x6C`, 32-byte tiles, tile index = row·64 + column, sector = row·3 + column).
  `cWorldView_initRowWalk` (`0x632A30`, thiscall (pos {+4 x, +8 y}, map data)) snaps the view's top-left corner
  (camera `0xAD5918`/`0xAD591C` minus view/2 and margins) to the tile lattice and finds its sector and tile; the
  even row position lives at `+0x970D4` (x, y float, tile, sector, steps int16), the odd one (+48, +24: one tile
  column further) at `+0x970E4`. `cWorldView0_render` calls `renderTileRow` for both, then steps each one tile row
  down (tile + 0x41, y + 48; at the sector's last row sector + 3, last column + 1, corner + 4). Inside the row
  (`0x62D3B2`) a tile step right is tile - 0x3F, x + 96, crossing to sector - 3 / - 2 / + 1. Row length is
  `+0x96AC8` (view width / 96 + 12); ground is drawn only for columns `+0x96AB8` - 1 .. length - `+0x96ABC` + 1
  (both 6), the rest are margins for objects. The walked area is the view plus 6 tiles left, 5 rows up and 19 rows
  down; the original view always lies inside the loaded sectors. A large zoomed-out view does not near a sector
  edge (at 2560x1440 zoom 2 the visible part still fits, the margins do not): the corner then is in no sector,
  `initRowWalk` stores sector -1 with a tile index computed from the offset tables at index -1, the steps turn
  that into a valid sector with a negative tile index, and `renderTileRow` (which checks the index only against
  `> 0xFFF`) read before the tile table: crash at `0x62B203` (e.g. leaving a town to the north). Rows leaving the
  loaded sectors to the right or bottom stepped into wrong sectors. SacredBild anchors each frame's walk in the
  192x192 loaded tiles (the game's corner, or a lookup next to the camera stepped back in whole tiles) and runs
  each row only over its loaded part, shifting x, tile, sector, length and the ground columns. `0x62D870` is an
  unreferenced copy of the row walk.
- Input: mouse events (`0x8950A8` down, `0x897248` up; x/y at +8/+0xC) carry the UI-space cursor.
  `cEngine_receiveEvent` (`0x618130`) hands them to the UI manager first (UI coordinates), then to the world
  mouse handler `0x617360` (thiscall (event, flag)), which picks with them; SacredBild converts the event to
  screen pixels for that call only.
- Hold-to-move: after 0.5 s of holding the button (`0x617360`, timer `0xAD4E9C`) the hero walks toward
  `cursor - (512, 384)` turned into an iso direction (`0x4FB620`, `lea reg, [mouse + 2*off - 0x200]`). That
  read is redirected to screen pixels, so the center is patched to W/2, H/2.
- Animated water/lava tiles (record type `0x90`/`0xA0`): `renderTileRow` appends 0x98-byte entries at
  `+0x3FF2C` (count `+0x80E3C`) with no bounds check; `cWorldView_drawWaterTiles` (`0x62DE70`) draws them after
  all rows (glow pass, tile pass) and sets the water ambience from their count and average position. Entry 1750
  lands on the count: a large water area fully zoomed out at 2560x1440 overwrote it and crashed in
  `renderTileRow` (`0x62B6EC`). SacredBild flushes ground, layers and water tiles between rows before either
  array fills up.
- Device calls from other threads: in game the main thread calls the device itself, ~3-4 times per second:
  `0x6285C0` (zoom: `GetTransform` + `SetTransform`) and the render flag setter `0x6435A0` (filtering, stage
  states). The proxy therefore keeps a lock (a spinlock: the render thread never pays a kernel wake-up).
- Tile layer records come from `0x6360E0`: a map cache of 0x1000 entries filled from the data file
  (fseek/fread per miss) with an O(n) LRU scan per insert when full. A zoomed-out high-resolution view needs
  more entries and thrashed every frame (~140 ms); the limit is raised to 0x8000. The texture manager budget (`0x65EA20`, set by `initApp`) is not a limit on modern
  machines (computes to ~4 GB).
- In game the frame only clears Z (`cEngine_renderThreadRun`); the ground has to cover the screen.
  SacredBild clears the target before `cWorldView0_render`.
- Frame limit: before each flip `cEngine_renderThreadRun` calls `0x60AA90` (cdecl (double, fps)) with
  `push 0x3c` = 60 unless global options `0x182CDBC` have `0x4000`; it spins with `Sleep(0)` until 1/fps passed.
  `cUI_Manager_runThread` calls it as well (menus). SacredBild's `FpsLimit` replaces the in-game value only.
- Fade: `cEngine_renderFadeOverlay` (`0x60E100`) draws a full-screen TL quad while engine flags
  (`+0x54`) have `0x20000` (fade out, then latches `0x80000` = black), `0x40000` (fade in) or `0x80000`.
  `cEngine_setFadeMode` and many script functions toggle them.

## Draw calls and batching

Measured at 1920x1200, zoomed out (2.0): ~9,600 draws per frame, 8,400 stage-0 texture switches, ~350
distinct textures, 21.6 ms of 30 ms world time inside `DrawPrimitive*`. Nearly every draw uses another
texture than the one before, so the game's own batching rarely gets past one quad.

- Ground quad batcher at `cWorldView + 0x86890`: `0x629260` (thiscall (device, 4 vertices)) culls against
  0..1024 / 0..768, appends a quad (FVF `0x244`: XYZRHW, diffuse, two texture coordinate sets) and flushes at
  0x252 indices; `0x629420` flushes with one `DrawIndexedPrimitive` and is called whenever the texture changes
  (`0x6293A0` one texture, `0x6293E0` two). The detail histogram shows mostly 6-index draws: one quad each.
- `cTileRenderer` (`0x61E810`/`0x61FAC0`, vertex buffer) is not used in game (no `DrawPrimitiveVB` calls).
- Sprites: `dxDriver7_drawTexturedQuad` and the object passes draw FVF `0x1C4` strips and small indexed lists.
- Render state cache: `0x643560` (thiscall (flag, on)) skips redundant changes of a flag word at device
  wrapper `+4`; `0x6435A0` applies them. Flags: `1` ZENABLE, `2` CULLMODE CCW/none, `4` ALPHABLENDENABLE,
  `8` SRCBLEND one/srcalpha, `0x10` DESTBLEND one/invsrcalpha, `0x20` LIGHTING, `0x40` ZFUNC greater/lessequal,
  `0x80` SPECULARENABLE, `0x100` ZWRITEENABLE, `0x200` COLORVERTEX, `0x400` FILLMODE solid/wireframe, `0x800`
  linear/point filtering (stages 0+1), `0x1000` stage 0 addressing wrap/mirror, `0x2000`/`0x20000`/`0x40000`
  texture stage setups for ground layers, `0x4000` ALPHATESTENABLE, `0x8000` texture transform (count 2),
  `0x10000` stencil. Sprites toggle alpha test and Z writes around each draw; the values at draw time repeat.
- No `ApplyStateBlock` calls exist; `GetRenderState` is used by `cTileRenderer_flush` and a few others.
- Textures come from `dxDriver7_createSurface` type 1: `DDSCAPS_TEXTURE` + `DDSCAPS2_TEXTUREMANAGE`, mostly
  256x256 ARGB4444, some ARGB8888, no mipmaps or color keys. The texture manager (`0x65F010`) loads and
  evicts them on the render thread while drawing.

SacredBild batches the world view (`src/render/batcher.*`, active inside `cWorldView0_render`): state calls
are recorded; at each pretransformed triangle draw the recorded state is compared with the device, and
draws that match are appended to one pending triangle-list `DrawIndexedPrimitive`. Small textures are used
through copies in 4096x4096 atlas pages with a one-texel gutter (`src/render/atlas.*`), with texture
coordinates remapped, when the draw's coordinates stay within half a texel of the texture. The gutter holds
the opposite edge for wrap addressing and repeats the edge for clamp/mirror (identical that close to the edge).
Copies are refreshed when a texture's uniqueness value changes; destroyed textures are noticed through
private data (`DDSPD_IUNKNOWNPOINTER`). 3D models (strided draws), Clear, EndScene and back buffer locks
draw the pending batch first. Merged draws go out with `D3DDP_DONOTCLIP` (`BatchNoClip`): after merging,
Direct3D still spent about 20 ns per vertex (200k vertices per zoomed-out frame), and the GPU clips anyway.
First measurement (1920x1200, zoom 2.0): 9,500 game draws -> ~780 device draws (~390 batches, the rest
3D model parts), world view 30.5 ms -> 14.5 ms. The frame rate stays at 60 because of DDrawCompat's
`FpsLimiter = flipstart(60)` in the user's DDrawCompat overlay config.

Profile after batching (2560x1440, zoom 2.0, ~46 fps, render thread): granny.dll 23 %, SacredBild 15 %,
AMD user-mode driver 12 %, DDrawCompat 10 %, ntdll 9 %, msvcrt 6 % (memcpy), d3dim700 5 %, sacred.exe 16 %.
- 3D characters are about half the frame. Per creature `cObject3D_drawModel` (`0x44ABA0`) runs a visibility
  test `0x405810` (reads world/view/projection back with `GetTransform`), `cGranny_render` (`0x405CD0`: per
  mesh piece `GrannyLockNextRenderingState`, SetTexture, SetTransform(WORLD), `DrawIndexedPrimitiveStrided`
  FVF `0x112`/`0x152`, lighting on) and `cGranny_renderShadow` (`0x407030`: the same meshes flattened through
  a shadow matrix, FVF `0x142`, stencil flag `0x10000`). `0x401920` calls `GrannyAdvanceTime` per model
  instance (~10 % of the thread); the skinning inside the lock calls is about as much. Granny is 1.x (2002 API:
  `GrannyLockSequenceForRendering`, `...RenderingState`).
- `GetTransform` through d3dim700/DDrawCompat/driver cost ~5 %: SacredBild answers it from the last
  `SetTransform`.
- `0x6404C0` (5 % exclusive) is the `std::map` find of the map data's record caches `0x6360E0` (ground layer
  records, render thread) and `0x635F50` (64-byte records, 85 call sites incl. game logic). Only these two
  functions insert/evict; the constructor `0x6336F0` and `0x633F80` (unload) create/clear the maps.
  `RecordIndex` puts a `gtl::flat_hash_map` (id -> node) in front, behind a reader/writer lock; it is dropped
  whenever the map's head or size differs from what the index last saw (eviction, clear, new object).
- `0x66FCE0`/`0x66FCF0` wrap `WaitForSingleObject`/`ReleaseMutex` on kernel mutexes (~2 %).
- With two 4096 atlas pages, ~1,100 batches per frame ended on a switch between the pages (430 textures per
  frame do not fit one page): pages are 8192 now.

Model batching (`BatchModels`): untransformed draws (characters FVF `0x112`/`0x152`, shadows `0x142`,
~400 per zoomed-out frame) are gathered from their strided streams into the batch and drawn from a vertex
buffer instead of `DrawIndexedPrimitiveStrided` (D3D time -2 ms). They merge only with the same world matrix and
state; WORLD is only recorded while batching, and view/projection, lights, material, clip planes and T&L-only
render states end a pending model batch. A first version moved model vertices to world space on the CPU to
merge across world matrices: each character switches lighting and FVF between its shadow and its model, so only
~70 of ~400 draws merged while the transform of ~180k vertices cost ~2.5 ms. Measured with it: Granny 26 % ->
13 % of the render thread with `AsyncAnimation` (worker never waited for: `game waited` ~0.01 ms).

`GrannyAdvanceTime` is called once per frame by `0x401920` (only caller, result ignored), from
`cEngine_renderThreadRun` before the world view. `AsyncAnimation` runs it on a worker thread; all 54 Granny
imports of the exe are patched with stubs that first wait for the worker. granny.dll has no TLS. With
DDrawCompat's default `CpuAffinity = 1` the process affinity mask still covered all 16 CPUs here and the worker
overlapped; the log reports the mask in case a setup really is limited to one CPU. Limitation: a raw (non-atlas) texture the game locks and changes in the
middle of the world pass would show its new content in draws batched before the change.

## UI (`cUI_Control2` / `cUI_Window2` / `cUI_Manager`)

`cUI_Control2` (vtable `0x895488`): `+0x10` flags (bit 0 visible), `+0x24` x, `+0x28` y, `+0x2C`/`+0x2E`
width/height (int16), `+0x30` name (char[0x20], uninitialized for default-constructed controls),
`+0x50` parent. Render = vtable `+0x14`.

`cUI_Window2` (vtable `0x8951D8`, type descriptor `0x9DE188`) adds a child vector at
`+0x78/+0x7C/+0x80`. `cUI_Window2_addChild` (`0x727120`) only pushes into that vector, it does not
set the parent pointer; most constructors pass the parent to the control's constructor instead (the
inventory's border pieces are created at negative y relative to the window), so children are usually
relative. `cUI_Control2_getAbsoluteRect` (`0x731C70`, 154 calls) adds parent offsets when `+0x50` is set, and
most hit tests and tooltips go through it. Window code still draws and tests at fixed 1024x768 positions in
places: the taskbar's center ornament at (512, 710) (`0x6E3D20`, `0x6E3BB0`), its hit test (`0x6DFB10`: x 384..640,
y 676..768), the minimap's radar ping at (975, 70) (`0x6D6E70`), the party arrows along the screen edges
(`0x6CFF70`: 8..1008 x 8..752), the megamap's scrolling at x 1023 / y 767 (`0x6C76A0`).

Virtual functions (thiscall, vtable byte offsets): `+0x10` receiveEvent(cEvent*) -> bool, `+0x14` render(device),
`+0x1C` isInside(x, y) -> bool, `+0x24` show(bool), `+0x30` update, `+0x34` / `+0x38` ESC / Enter (from
receiveEvent), `+0x44` render2(device, ?, ?) on blacksmith, merchant, net portraits (and menu windows). Mouse
button events: cEventMouseDown (vtable `0x8950A8`, type 2) and cEventMouseUp (`0x897248`, type 3), x/y at
`+8`/`+0xC`; there is no mouse move event, hover reads the cMouse. The manager calls `+0x10` of each window in
turn (`cUI_Manager_receiveEvent` `0x757120`), `+0x14`/`+0x44` to draw and `+0x1C` from
`cUI_Manager_isCursorOverUi`.

Construction: `cRect_ctor(x, y, w, h)` (`0x6A55F0`) builds the rect passed by value to
`cUI_Window2_ctor` (`0x724070`), which calls `cUI_Control2_ctor(name, x, y, w|h<<16, parent, flags)`
(`0x731420`). Several derived constructors reposition the window afterwards (inventory -> (0,388),
equipment -> (656,388), mercenaries -> (932,0), ...).

`cUI_Manager` (vtable `0x8957D0`): `cUI_Manager_createGameWindows` (`0x7593D0`) creates the in-game
windows into `+0x80..+0xD8` (taskbar, inventory, equipment, blacksmith, merchant, megamap, overview map,
minimap ("UI_WND_MERC": minimap and party portraits, 932,0 92x676), stats, console, questbook, escape menu,
purchase, master, savegame, options, chest, horse, net info, net portraits, character, cube, trade; rects and
offsets in `Sacred::UiManager`). The shop windows (blacksmith, merchant, master, chest, cube, trade) are 640 wide
at 0,0, next to the inventory at 0,388. `cUI_Manager_render` (`0x7587B0`, arg: device) draws the
cinematic letterbox bars (float immediates 1024/768) and all windows. Full-screen menus are separate
`cUI_Window2`s with rect (0,0,1024,768) or (0,0,1023,767). In game (mode bits `0x04` and `0x40`) a visible savegame (`+0xAC`),
options (`+0xBC`) or character (`+0xD0`) window is drawn alone; the megamap (`+0x94`) alone with the tutorial
hints and popups; otherwise all HUD windows. The escape menu (`+0xB0`) and popups follow in every case.

Input: `getClientCursorPos` (`0x66E500`, cdecl `(HWND, POINT*)`) is the cursor read for the game; the
window procedure is `sacredWndProc` (`0x8122A0`, class "Sacred" registered by `0x813240`). It calls
`getClientCursorPos` for WM_MOUSEMOVE (then `cMouse_setPosition`) and the button messages, whose events
carry that position. `cEngine_scrollViewAndWarpCursor` (`0x610650`) also uses GetCursorPos/SetCursorPos
(screen pixels) to keep the cursor on its world spot while the view scrolls. (`0x664910` creates the
debug "LogWindow", not the game window.) `cMouse_instance` (`0x6550F0`) holds x/y at `+4/+8` and optional clamp
bounds at `+0x14..+0x20`; `cMouse_getX`/`getY` (`0x6559E0`/`0x6559F0`) are thiscall getters,
`cMouse_renderCursor` (`0x6555E0`, `(device, flag)`) draws the cursor. UI code reads `+4/+8` directly or
through events. World code reads the mouse in `0x611F10`, `0x617360` (getters) and `0x4FB620`,
`0x60F1D0`, `0x610420`, `0x611F10`, `0x627260` (instance + direct field reads).
`cInventoryEntry_render` (`0x5DC3F0`) and the getter callers in `0x6B6330`, `0x6DA0A0`, `0x6DBCD0`,
`0x6DD720` are UI. `cEngine_updateWorldCursor` (`0x611F10`, render thread, before the UI) reads the mouse
once and passes it both to `cUI_Manager_isCursorOverUi` (`0x75A370`, UI coordinates; if true no world
pick happens) and to the world pick, so SacredBild converts back for that one call (`0x611F83`).

Frame loops: `cUI_Manager_runThread` (`0x756940`, menus): `Clear(target|z)`, `beginScene`,
`getClientCursorPos`, `cUI_Manager_render`, `cMouse_renderCursor`, `endScene`, `flip`; while loading it
calls `dxDriver7_drawLoadingScreen` via `0x759000` instead. `cEngine_renderThreadRun` (`0x60E3F0`,
in-game): world, overlays, `cUI_Manager_render`, cursor. `playVideo` (`0x6A0C60`, thiscall, 5 args):
DirectShow video into a 1024x512 texture drawn as a (0,0)-(1024,768) TL quad with its own frame loop.

Popups (tooltips, hints; class vtable `0x894B3C`, constructor `0x6E6400`) live in the manager's vector at
`+0x124/+0x128` and are drawn by `0x7586A0` after the windows. Callers set the popup's x/y and then its text with
`0x6E6AE0` (string) or `0x6E6BF0` (text id), which flags a new layout (`+0x154` bit 0x40); the popup's render
(`0x6E7300`) then lays it out (`0x6E7730`: centered on x/y, clamped into 16..1008 x 16..752, or centered on the
screen). Show helpers: `0x75AC70` / `0x75AD10` (x, y, text, ...); the equipment window sets x/y itself
(`0x6B73A0`). `cUI_Manager_showHelp` (`0x75ADD0`, thiscall (device, short screen), from `cUI_Manager_render` every
frame) is the help screen: with manager flag `0x200` (H key) it puts the help popups (indices: uint16 array at
manager `+0x56`) at fixed 1024x768 positions (tables at `0x17EC958..0x17ECB38`, x, y, w|h<<16; text ids from
`0x9EA544`) and sets their texts, flags `0x201`; without it the hints "[H] shows or hides the help screen" and
"[TAB] for the overview map" at (425, 100) and (425, 170) (manager flags `0x400`, `0x800`). The screen
follows the open windows: 1 none (general hints), 2 inventory (entries next to the stats window, over inventory,
equipment and taskbar), 3 blacksmith, 4 combo master (`0x6D11C0`), 5 merchant, 6 world map, 7 rune exchange at the
master (`0x6D11A0`). Every screen's first entry is "[H] shows the help screen".

The cMouse (`cMouse_instance` `0x6550F0`, a 0x70-byte singleton behind `0xCDBADC`): `+4/+8` position, `+0xC/+0x10`
the drawn position (copied and clamped to `+0x14..+0x20` by `0x655670`), `+0x64` cursor image, `+0x68` the item
held by the cursor (`0x6556E0` get, `0x655710` set, `0x655880` clear). Reads of the position: 89 sites, 17 in
world code, the other 72 in the UI (55 `cMouse_instance()` followed by reads of +4/+8 only, 6 `getX`, 6 `getY`,
5 `getCursorPos` `0x6559A0`), two of them outside the UI code range (`cInventoryEntry` `0x5DC2D0`, `0x5DC3F0`).

### SacredBild UI canvas

Per-window re-anchoring does not work: children are absolute, many renderers draw at absolute
1024x768 positions, and windows are "hidden" by parking them off-screen. Instead the whole UI stays in
1024x768 space and is drawn into a centered canvas (`src/game/ui_canvas.*`):

- UI scopes: `cUI_Manager_render`, `playVideo` (confined to the canvas) and `cMouse_renderCursor`
  (overlay: mapped, but not culled or clipped, so the cursor also shows beside the canvas).
  `renderSavePortrait` suspends it.
- In a scope the device proxy sets the viewport to the canvas (3D UI elements follow), maps
  pretransformed vertices `x*s + left`, culls draws entirely outside 1024x768 (parked windows) and clips
  axis-aligned quads to the canvas. Clear rects are mapped; GetViewport returns the virtual viewport.
- Two placements: in game (`[UI] Scale`) and menus (`ScaleMode=InGame`: as large as fits; `Full`: the in-game
  one), picked by the UI manager's mode (`+8` bit `0x04` in game; `0x03` in the menus, `0x4C` in game, `0x43`
  leaving it); an open full-screen window in game takes the menus' placement. The outermost UI scope keeps the
  placement it started with.
- Loading screen (`dxDriver7_drawLoadingScreen`, GDI on the back buffer's DC, then `dxDriver7_flip`, only when the
  progress bar moved): SacredBild puts a 1024x768 system memory surface in the driver's back buffer slot (`+0xBC`)
  for the call; the flip hook puts the back buffer back and Blts the surface scaled into the menus' canvas.
- UI images are rects in 256x256 sheets: records of 0x54 bytes from `0x9EAEB8` (`+0` id, `+4` type, `+8` TGA
  name, `+0x28` texture, `+0x2C..+0x38` x0, y0, x1, y1 in pixels at load, `+0x4C`/`+0x50` width/height).
  `0x760E60` makes x1/y1 exclusive and turns them into `u0 = x0/256`, `u1 = (x1 + 0.5)/256` (same for v): drawn
  1:1 with point sampling, pixel centers (whole coordinates) hit texel edges on the left and texel centers on the
  right. Scaled or bilinear, the edge pixels blended up to half of the neighbouring sheet image in (dark seams
  between tiles); the proxy rewrites the texture coordinates of axis-aligned textured UI quads so pixels sample
  at their middle and the outermost ones stay inside the image's texels (`DeviceProxy::fitTexels`).
- `getClientCursorPos` returns canvas (virtual) coordinates, so cMouse and all UI hit tests are virtual;
  the world's mouse reads listed above are redirected to versions returning physical coordinates.
- Diagnostics: draws in a UI scope that extend beyond 1024x768, or that the proxy cannot map (3D, VB,
  strided), are logged once per call site; changes of the engine fade flags and the UI manager mode
  (`+8`: `0x10` cinematic) are logged as `State:` lines.

Anchored HUD (`src/game/ui_anchor.*`): moving the windows themselves would miss the fixed positions listed
above, so each anchored window keeps its 1024x768 coordinates and runs in a frame, a copy of the 1024x768
space placed against a screen edge. The frame is per thread (`UiCanvas::FrameScope`); the proxy maps draws into
the current frame and confines them to its rect; the 72 UI reads of the cursor return it relative to the
current frame. The vtable slots `+0x10`, `+0x14`, `+0x1C`, `+0x24` (and `+0x44` where it is render2) of the
anchored windows' classes point at thunks that enter the window's frame and shift mouse button events and
isInside arguments into it (all classes are single inheritance; the slot after a vtable's last entry is the
next one's RTTI pointer, which bounds the patch). Popups take the frame in which their text was set and are
drawn unconfined. The cursor itself, the manager's own drawing and every other window stay in the canvas.
Window positions come from `[UI.Layout]` (X,Y in 0..4096 of the room around the 1024x768 layout). Stats (656,0
256x420) and equipment (656,388 256x256) form one column; by default they share the top-right frame with the
minimap. The taskbar's level-up button ("+", embedded control at taskbar `+0x18C`, absolute 898,10 without a
parent, set by the taskbar setup `0x6E4BA0`) lies on the stats window's close button and is covered by it while
the window is open; it is moved within the taskbar's (unconfined) frame by the offset between the two frames.
The taskbar draws it (`+0x14`), highlights it while flag 0x400000 is set (`0x731B40`, non-virtual, reads its rect)
and hit-tests it through `+0x1C` / `+0x10`. The item held by the cursor is drawn by every window that takes items
(`cInventoryEntry_render` `0x5DC3F0` with (device, 1), at `getCursorPos`); it runs unconfined. The effects list
behind the portrait's "+" is a popup at (16, 16) with the clamp flag (`0x6D7900`): the game's way of saying top-left
corner. Popups are drawn unconfined, and their layout (`0x6E7730`, hooked) runs without the game's clamp and
centering (the flags' bits are cleared for the original, the children are laid out afterwards): a popup the game
would push against an edge of its 1024x768 screen goes against that edge of the real screen (in the popup's
frame), centered ones go to the screen center. The help screen sets all its texts in the canvas; after
`cUI_Manager_showHelp` each entry that explains a window gets that window's frame (a table per screen and entry), the
inventory screen's "[H]" hint (20, 80) the screen's top-left corner, general hints and the world map's stay in the
canvas. Untextured translucent black quads over the whole 1024x768 screen
are drawn over the whole screen: the escape menu's background (`0x6BB030`, 0x6F000000) and the dimmed background
of message boxes (windows with flag 0x800, `cUI_Control2_render`, 0x9F000000). The stats window's close button is an embedded control at `+0x210` named `UI_STAT_BTN_BASE`
(stats-relative 242,10, parent set); the game flags controls of that name with 0x20000 on level-up (`0x57EEF3`), and
the stats window's show (`0x6A5C60`) messages them as well. `[Debug] UiTrace` logs a frame's UI draws with their frame
and the calling code (a stack scan for return addresses into sacred.exe) for finding the rest.
Known gaps: the party arrows and the cinematic bars stay in the canvas.

## Things that read the back buffer

| Function | Behavior at > 1024x768 | SacredBild |
|---|---|---|
| `cEngine_captureInternal` (`0x613710`), savegame thumbnail | copies `desc.dwHeight` rows into a 1024x768 buffer: **heap overflow** | `lockBack` hook hands it the centered 1024x768 (return address `0x61374B`) |
| `renderSavePortrait` | size-aware (centered region) | projection constants patched; runs outside the UI canvas |
| `captureScreenshot` (`0x648900`) / `writeTga1024x768` (`0x6483F0`) | screenshots cropped to top-left 1024x768, video capture garbled | not yet handled |
| `debugCaptureCharacter` (`0x6F5190`) | developer tool, reads fixed offsets inside the surface | ignored |

## Hard-coded 1024x768 elsewhere

UI functions comparing against 1023/767 or using 768 offsets (`0x6C7580`, `0x6C76A0` megamap
scrolling, `0x6C99E0`, `0x6CC950`, `0x6E7730` taskbar, `0x70FC70`, `0x759D60`, `0x75A870`) and windows
positioned at runtime (`0x75AC70`, `0x75AD10`) are correct inside the UI canvas and need no patches.

## Networking: LAN games

Game traffic goes through Ascaron's `tincat2.dll` (TCP and UDP sockets, imported via WSOCK32). Finding LAN
games is the game's own code on top of Winsock:

- **Hosting**: creating a game (`cUI_NetLan`, action 5) writes `GameServer.cfg` (`writeGameServerCfg`
  `0x7EE360`: `NETWORK_IP_ADDRESS`, `NETWORK_PORT_LISTEN`, session name, ...) and starts `GameServer.exe` with
  `CreateProcessA` (`0x7D8427`, no window).
- **gameserver.exe** (Ghidra `/gameserver.exe`, timestamp `0x451BBDBF`): app object `*0x634238`, network object
  at app `+0x40`. `cNetServer_initNetwork` (`0x4E8AB0`) opens the ping socket (`+0x1A8`): UDP, `SO_BROADCAST`,
  `connect()`ed to `255.255.255.255:<NETWORK_PORT_LISTEN>`. `cNetServer_sendAnnouncement` (`0x4E99A0`, called
  periodically and when players join or leave) fills the plain announcement at `+0x596C`, runs it through
  `CompressMemory` (`0x4918B0`) and `send()`s it — the exe's only `send` call.
- **Announcement** (0xAE bytes): `+0` u16 version check (from `TinCat_GetBuildNumber`), `+2` u16 gameserver
  TCP port, `+4` u32 IPv4 address in host byte order, `+8` u32 flags, `+0xC` u8 players, `+0xD` u8 max players,
  `+0xE` wchar_t name[80]. The address is `0x6341B8`: the first address `detectLocalIps` (`0x495350`) found, or
  `NETWORK_IP_ADDRESS` if it is one of them. `detectLocalIps` reads the IP address table through SNMP
  (`inetmib1.dll`) and keeps at most **three** addresses (skipping 0, 127.x and x.x.x.254/255); without SNMP it
  falls back to `gethostbyname`, one address. sacred.exe has the same code (`0x803F40`, table at `0x182CC70`).
- **Wire format** (`CompressMemory` / `UncompressMemory` `0x800A60`): u32 header = payload size, bit 27 set if
  not compressed. Payloads under 0x50 bytes are XOR-chained (`plain = (prevCipher + 0xB5D6C7A3) ^ cipher`,
  seeded with the size, the last size % 4 bytes raw), larger ones zlib. The receiver accepts both for any size.
- **LAN list** (`cGCclass`, `*0x182CB70`): `cGCclass_initNetwork` (`0x7D2410`) binds a UDP socket (`+0x14`,
  `SO_REUSEADDR`) to `INADDR_ANY:NETWORK_PORT_LISTEN` (2005). `cGCclass_pollLanGames` (`0x7D2E20`) drains it
  with `select` / `__WSAFDIsSet` / `recvfrom` (their only callers), accepts packets that decode to 0xAE bytes,
  keys games by (address, name) and drops a game after 5 s without an announcement. The sender address is
  ignored: `cGCclass_joinLanGame` (`0x7D3530`) connects to the address in the announcement.

Over a VPN this fails twice: Windows sends a limited broadcast through the adapter with the best route only (and
L3 VPNs carry no broadcasts at all), and the announced address is often not the VPN one. SacredBild injects
itself into the gameserver (Detours `DetourCreateProcessWithDllExA` from a `CreateProcessA` IAT hook) and
replaces the `send` import: the game's broadcast carries the address of the adapter it leaves on, every other
adapter gets a subnet broadcast with its own address (XOR format), and players that subscribed at UDP 2105 get
the plain announcement and fill in the address they received it from (`src/net/`).

## Networking: game connection and the "data flow" (ISDN / LAN) setting

Game traffic is TinCat over **TCP** (`tincat2.dll`, Ghidra `/tincat2.dll`; the auto-analysis there fails, so
functions exist only where they were created by hand). TinCat has its own UDP broadcast lobby, which Sacred does
not use. `cGCclass_initNetwork` creates the TinCat API once when the multiplayer menu is set up
(`0x7D7D00`) and configures a `TinCatValues` object (vtable `0x1003CE60`). Its fields match the keys of TinCat's
`[tincat]` config reader (`0x1000A300`):

| Sacred call | TinCat field | Key | Value |
|---|---|---|---|
| `+0x08` | profile copy | (`TinCatValues_init`: client profile table `0x100491E0`, server `0x10048FE0`) | profile 0 |
| `+0x2C` | `+0x44/+0x48` | `drv_writer_sleep` (+activate) | 5 ms |
| `+0x34` | `+0x3C/+0x40` | `drv_reader_sleep` (+activate) | 5 ms |
| `+0x3C` | `+0x4C/+0x50` | `drv_serverloop_sleep` (+activate) | 100 ms |
| `+0x44` | `+0x14` | `srv_sendlogonrequest` | 15000 ms |
| `+0x4C` | `+0x34` | `usr_logoncheck` | 10000 ms |
| `+0x54` | `+0x20` | `srv_checkdeadcon` | 70000 ms |
| `+0x5C` | `+0x28` | `srv_sendalive` | 30000 ms |
| `+0x68` | `+0x54` | `drv_disable_nagle` | client: data flow == LAN; server: always |

**Data flow** (`NETWORK_SPEEDSETTINGS`, `cGCclass +0x40414`; the options' radio buttons store 1 = MODEM/ISDN,
2 = LAN, 0 = not set; `setNetSpeedMode` `0x7ED1B0` stores it and sends message 0x21). It does three things:
- client: TCP_NODELAY on the TinCat connection for LAN only, decided once at `cGCclass_initNetwork`
  (`cmp ebx, 2; sete al` at `0x7D294E`; SacredBild's `[Net] NoDelay` makes it 1 in both modes);
- it travels in the first-contact message (`cGCclass +0x40064`, copied before `cGCclass_joinLanGame`); the
  server keeps it in the player record (`net + 0x1D4 + slot * 0x518`) and updates it on message 0x21;
- server: object simulation is distributed to clients. `setObjectOwner` (`0x4D6950`) sets a creature's owner
  slot (`+0x39`, recursively for its children) and sends message 0x171. When a creature needs a new owner, the
  main loop (`0x4C5082`) walks the players that see it and skips ISDN players
  (`cNetServer_isPlayerIsdn` `0x4DCDC0`) for creatures with flag `0x20` at `+0x200` (ambient NPCs). The player
  list draws ISDN players in another color.

Community reports: joining in LAN mode fails over the internet ("IP: Cannot Connect!", connect time-outs),
joining in MODEM/ISDN works and switching to LAN afterwards is fine; ISDN hides most ambient NPCs. Switching after
the join keeps TCP_NODELAY off (TinCat is not re-initialised) but makes the player an owner candidate.

Server time-outs (`cNetServer_watchdogThread` `0x4DBCB0`, 5 ms loop, times from `0x6340C8`; player record =
`net + 0x1B0 + slot * 0x518`): first contact must arrive within **5 s** of the connection (`+0x60`, checked every
second; SacredBild makes that `[Net] JoinTimeout`, 30 s by default); loading may take 4 min (`+0x74` set, `+0x64` last receive); in game, three 30 s ticks without any data
(`+0x6C..+0x6E`, cleared by `cNetServer_onReceive`) kick the player; idle warning at 14 min, kick at 15 min
(`+0x68` last activity, not the host).
