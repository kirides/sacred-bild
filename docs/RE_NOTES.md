# Sacred Gold (DE build) renderer notes

Target: German `sacred.exe` 2.0, PE timestamp `0x451BBE74`, image base `0x400000`.
Ghidra program: `/sacred.exe (DE)` (functions below are named there). An English build
(`/Sacred.exe`, timestamp `0x452F85C7`) is in the same project; around the renderer its addresses are
ENG + `0x130`, elsewhere the offset differs. All addresses below are DE.

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
  2D sprites draw with Z off, so their layering against the 3D characters is purely draw order.
- `cWorldView_screenToWorld` (`0x62A0A0`, vtable slot 6): `(mouse - backbuffer/2) * zoom + camera`, uses
  the real back-buffer size.
- Ground layers: `renderTileRow` draws each tile's base through the quad batcher (`+0x86890`, flush
  `0x629420`) and copies tiles with blend layers into an array at `+0xB64` (0x90 bytes each, list heads at
  `+0x3E3C4`, count `+0x3FF1C`, capped at `0x6D5`); `cWorldView_drawTileLayers` (`0x62D530`) draws them after
  all rows. A zoomed-out high-resolution view overflows the cap (later rows lose their blends: staircase
  edges), so SacredBild flushes batcher + layers between rows when the array is nearly full.
- Input: mouse events (`0x8950A8` down, `0x897248` up; x/y at +8/+0xC) carry the UI-space cursor.
  `cEngine_receiveEvent` (`0x618130`) hands them to the UI manager first (UI coordinates), then to the world
  mouse handler `0x617360` (thiscall (event, flag)), which picks with them; SacredBild converts the event to
  screen pixels for that call only.
- Hold-to-move: after 0.5 s of holding the button (`0x617360`, timer `0xAD4E9C`) the hero walks toward
  `cursor - (512, 384)` turned into an iso direction (`0x4FB620`, `lea reg, [mouse + 2*off - 0x200]`). That
  read is redirected to screen pixels, so the center is patched to W/2, H/2.
- Tile layer records come from `0x6360E0`: a map cache of 0x1000 entries filled from the data file
  (fseek/fread per miss) with an O(n) LRU scan per insert when full. A zoomed-out high-resolution view needs
  more entries and thrashed every frame (~140 ms); the limit is raised to 0x8000. The texture manager budget (`0x65EA20`, set by `initApp`) is not a limit on modern
  machines (computes to ~4 GB).
- In game the frame only clears Z (`cEngine_renderThreadRun`); the ground has to cover the screen.
  SacredBild clears the target before `cWorldView0_render`.
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
draw the pending batch first. Limitation: a raw (non-atlas) texture the game locks and changes in the
middle of the world pass would show its new content in draws batched before the change.

## UI (`cUI_Control2` / `cUI_Window2` / `cUI_Manager`)

`cUI_Control2` (vtable `0x895488`): `+0x10` flags (bit 0 visible), `+0x24` x, `+0x28` y, `+0x2C`/`+0x2E`
width/height (int16), `+0x30` name (char[0x20], uninitialized for default-constructed controls),
`+0x50` parent. Render = vtable `+0x14`.

`cUI_Window2` (vtable `0x8951D8`, type descriptor `0x9DE188`) adds a child vector at
`+0x78/+0x7C/+0x80`. `cUI_Window2_addChild` (`0x727120`) only pushes into that vector, it does **not**
set the parent pointer: **children keep absolute screen coordinates**, laid out for 1024x768.
`cUI_Control2_getAbsoluteRect` (`0x731C70`) only adds parent offsets when `+0x50` is set.

Construction: `cRect_ctor(x, y, w, h)` (`0x6A55F0`) builds the rect passed by value to
`cUI_Window2_ctor` (`0x724070`), which calls `cUI_Control2_ctor(name, x, y, w|h<<16, parent, flags)`
(`0x731420`). Several derived constructors reposition the window afterwards (inventory -> (0,388),
equipment -> (656,388), mercenaries -> (932,0), ...).

`cUI_Manager` (vtable `0x8957D0`): `cUI_Manager_createGameWindows` (`0x7593D0`) creates the in-game
windows into `+0x80..+0xD8` (taskbar, inventory, equipment, blacksmith, merchant, megamap, overview map,
mercenaries, stats, console, questbook, escape menu, purchase, master, savegame, options, chest, horse,
net info, net portraits, cube, trade). `cUI_Manager_render` (`0x7587B0`, arg: device) draws the
cinematic letterbox bars (float immediates 1024/768) and all windows. Full-screen menus are separate
`cUI_Window2`s with rect (0,0,1024,768) or (0,0,1023,767).

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
- `getClientCursorPos` returns canvas (virtual) coordinates, so cMouse and all UI hit tests are virtual;
  the world's mouse reads listed above are redirected to versions returning physical coordinates.
- Diagnostics: draws in a UI scope that extend beyond 1024x768, or that the proxy cannot map (3D, VB,
  strided), are logged once per call site; changes of the engine fade flags and the UI manager mode
  (`+8`: `0x10` cinematic) are logged as `State:` lines.

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
