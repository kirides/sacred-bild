# Sacred Gold reverse engineering notes

Target: the English GOG `Sacred.exe`, PE timestamp `0x452F85C7`, image base `0x400000`; Ghidra program
`/Sacred.exe (GOG)`. All addresses below are this build's; function names are the ones in
`tools/data/functions_eng.tsv`. The German `sacred.exe` 2.0 (timestamp `0x451BBE74`) has the same code at other
addresses: around the renderer DE = ENG + `0x130`, elsewhere the offset differs. SacredBild supports both through
byte signatures (`tools/gen_sigs.py`; `tools/data/addresses.json` lists every patched address in both builds).

## Big picture

Sacred is *not* a CPU blitter at its core: it renders with **Direct3D 7 (TnL HAL)** through a thin
wrapper, `dxDriver7`. The isometric world is real 3D geometry under an orthographic projection; sprites
and UI are pretransformed (`XYZRHW|DIFFUSE|SPECULAR|TEX1`, FVF `0x1C4`) quads. The CPU cost comes from
issuing thousands of small draws: a measured in-game frame at 2560x1440 (zoom ~1) did about 6,100
`SetTexture` calls with 4,070 actual texture switches and spent ~10.3 ms in the world renderer. Zooming
out to 2.0 shows four times the area and multiplies that.

Two threads present frames:
- `cUI_Manager_runThread` (`0x757060`): menus. Clears target+Z, renders UI, software cursor, flips.
  Sleeps 20 ms per frame in menus (~46 fps).
- `cEngine_renderThreadRun` (`0x60E350`): in-game. Per frame: clear Z, `beginScene`, world view
  (`vtable[5]`, `cWorldView0_render`), label layout, overlays, UI (`cUI_Manager_render`), `endScene`, flip.

## dxDriver7 (global `g_pDxDriver` = `0xCDCA1C`)

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
| `0x815F70` | `initApp`: `cDxDevices_findMode(1024, 768, bpp, flags)` at `0x816C79`, then `dxDriver7_ctor` |
| `0x644130` | `cDxDevices_findMode` (4 args) returns a 0x84-byte mode record |
| `0x644830` | `dxDriver7_ctor` |
| `0x645260` | `dxDriver7_init` (3 args): DirectDrawCreateEx, cooperative level, surfaces, z-buffer, CreateDevice |
| `0x644AB0` | `dxDriver7_createSurface` |
| `0x644ED0` | `dxDriver7_createZBuffer` |
| `0x645E40` | `dxDriver7_flip`: windowed = `primary->Blt(back)`; also a CPU fade effect |
| `0x646150` / `0x6461C0` | `dxDriver7_lockBack` / `unlockBack` |
| `0x6463E0` | `dxDriver7_setFullViewport` |
| `0x646580` | `dxDriver7_drawTexturedQuad`: one `DrawPrimitive(TRIANGLESTRIP, 0x1C4, 4)` per sprite |
| `0x6468A0` | `dxDriver7_drawLoadingScreen`: GDI on the back buffer DC, 1024x768 layout |
| `0x646E20` / `0x646EA0` | `beginScene` / `endScene` (ref-counted) |

Main window: `0x813B90` registers class "Sacred" (`sacredWndProc` `0x812BF0`) and creates it with `CreateWindowExA`
as `WS_POPUP|WS_VISIBLE` (return address `0x813C87`); `dxDriver7_init` sizes it with `SetWindowPos`: the screen in
fullscreen mode, the mode's size centered on the primary screen when windowed. (`0x664690` creates the debug
"LogWindow", `WS_OVERLAPPEDWINDOW`.) `WM_CLOSE` quits (`0x817580`); windowed, `WM_SYSCOMMAND` goes to
`DefWindowProcA`. Starting a game (`cCommand_armaPlay::execute` `0x7561E0` posts `0x8002`) replaces the window
procedure with `0x811A20` through the delay-loaded `SetWindowLongA` (`[0xA23FB4]`): subclasses don't survive it.

## What the game needs from DirectDraw (SacredBild's Direct3D 9 backend, `src/ddraw9/`)

- Imports `DirectDrawCreateEx`, `DirectDrawCreate`, `DirectDrawEnumerateExA`. Interfaces it asks for: IDirectDraw7,
  IDirect3D7, the HAL / T&L HAL device GUIDs, and IDirectDraw + IDirectDrawSurface (version 1) for the video stream
  only. No gamma control, no other interface versions.
- Startup enumeration (`0x644260`): DirectDrawEnumerateExA, then per device DirectDrawCreateEx, QI IDirect3D7,
  GetCaps (DDCAPS size 0x17C), EnumDisplayModes, EnumDevices.
- `dxDriver7_init`, windowed: SetCooperativeLevel(hwnd, NORMAL | MULTITHREADED | FPUSETUP); primary
  (PRIMARYSURFACE | 3DDEVICE | VIDEOMEMORY, color-filled); back buffer `dxDriver7_createSurface(0, w, h, 0xd)` =
  3DDEVICE | VIDEOMEMORY, no pixel format; EnumZBufferFormats for 32 bits with 8 stencil bits (16 bits at 16 bpp,
  callback `0x644E60` matches flags, depth and stencil depth); the z-buffer is AddAttachedSurface'd to the back
  buffer; CreateDevice with the back buffer (three tries), then the primary; a clipper on the primary; Lock/Unlock of
  primary and back buffer only to read pitch and format. Fullscreen: SetDisplayMode, flip chain, Flip.
- Frames: windowed `primary->Blt(client rect in screen coordinates, back buffer, rect, DDBLT_WAIT)`.
  `IDirectDraw7::WaitForVerticalBlank` runs before every `dxDriver7_lockBack` and `+0x290` times per flip.
- `dxDriver7_createSurface` types: 1 = texture (TEXTURE + DDSCAPS2_TEXTUREMANAGE, pixel format from
  EnumTextureFormats; the callback `0x646F00` keeps the last format matching bit count and alpha mask); 0 with flag 2 =
  OFFSCREENPLAIN | SYSTEMMEMORY (fonts: GetDC/ReleaseDC for GDI text, Lock, then copied into textures); 3 = windowed
  primary + back buffer; 2 = fullscreen flip chain. GDI also draws the loading screen into the back buffer's DC.
- Granny textures: `0x401BE0` enumerates texture formats (callback `0x403F60` takes 4444, 565, 555, 1555, 888,
  8888, no FourCC) for `GrannyAllowTextureFormat`.
- Movies (`movie\*.wmv`, `0x6A16E0` picks them by id: ASCARON/PUBLISHER/OEM at startup from `initApp`, INTRO,
  EXTRO, act1-8, ...): `openMovieStream` (`0x6A0D80`, cdecl (path, IDirectDraw*, IAMMultiMediaStream** out)) creates
  amstream, adds the DirectDraw object as the primary video stream and the default audio renderer, opens the file.
  `playVideo` (`0x6A0EA0`, thiscall on the movie player: `+4` skip count, `+8` HWND; args IDirectDraw*, primary
  IDirectDrawSurface*, stream, 640, 480; returns -1 when skipped). With `COMPAT_VIDEO` it creates a 1024x512 texture
  (caps TEXTURE only), hands it to `IDirectDrawMediaStream::CreateSample` as IDirectDrawSurface and draws it as a TL
  quad after each `IDirectDrawStreamSample::Update`; without, amstream creates its own surface and the game Blts it
  to the primary. Its loop only peeks keyboard messages (removing them) and skips on ESC, space or a mouse button
  (`GetAsyncKeyState`). amstream doesn't work on SacredBild's emulated DirectDraw (no picture, then the window hung
  after skipping), so with the Direct3D 9 backend `openMovieStream` only records the file and `playVideo` plays it
  through Media Foundation, or through amstream on Windows' own DirectDraw (a system memory sample surface, copied
  into a texture) where Media Foundation can't (`src/game/movie.cpp`).
- `dxDriver7_beginScene` (`0x646E20`, ref-counted at `+0x1D8`) retries `IDirect3DDevice7::BeginScene` until it
  succeeds: a device whose BeginScene fails hangs the calling thread.

## World view (`cWorldView`, vtables `0x89209C`, `cWorldView0` `0x8920F8`)

- `+0xB60` float **zoom**, clamped to [`g_flZoomMin` 0.5, `g_flZoomMax` 2.0].
- `cWorldView_setZoom` (`0x628130`), `cWorldView_addZoom` (`0x628310`), `cWorldView_applyProjection`
  (`0x624E30`), vtable[7] `0x6288F0`: build `matrixOrtho(±267·zoom, ±200·zoom, -600, 2500)` from doubles
  `g_dOrthoLeft..Top` (`0x890E40..0x890E58`) and set it as the projection, then store an unzoomed copy
  in `g_unzoomedProjection` (`0x182ED70`) using push immediates ±267/±200.
  534x400 world units map to 1024x768 px.
- `g_unzoomedProjection` is not a render matrix: `FUN_00623940` (pixels -> world, `x * (2/P[0]) / 1024`),
  `FUN_00623b60` (world -> pixels, `* 1024 / (2/P[0])`) and their wrappers (`0x623910`, `0x6239C0`
  iso -> world, `0x623A80`, `0x623AD0`) use it to convert between world units and pixel offsets. Game logic
  (creature AI, spells, scripts, model placement) calls these everywhere, so the matrix and the 1024/768
  in them must stay unpatched: widening it scaled every 3D model's world position by W/1024, which made
  all characters vanish and broke world clicks and the intro script. Only the device projections set by
  `cEngine_ctor` and `0x60D740` (push ±267/±200, `SetTransform(PROJECTION)`) are widened.
- Object/creature passes (`0x62E410`, `0x62FF60`) cull screen positions against `0x4C8` / `0x3C8`
  (1024 + 200, 768 + 200), patched to W + 200 / H + 200. Characters are Granny 3D models
  (`cGranny_render*` around `0x402000-0x40FFC0`), placed in world space; ground and objects are TL sprites.
- `cWorldView_updateViewMetrics` (`0x624A00`): view size `+0x96AB0 = zoom·1024`, `+0x96AB4 = zoom·768`,
  tile columns `w/96+12`, rows `h/48+24`.
- `cWorldView0_render` (`0x632140`, arg: device): walks tile rows (`cWorldView_renderTileRow`
  `0x62AE90`), collects objects into growable vectors, then draws them (`0x629340`, `0x62D3C0`,
  `0x62E410`, `0x62DD00`, `0x62FF60`). Those passes cull projected positions against
  `0 < x < 1024.0f` / `0 < y < 768.0f` (`g_flScreenWidth1024` `0x8906F4`, `g_flScreenHeight768`
  `0x892040`).
- Ground tiles: `cTileRenderer_instance` (`0x620090`, object `g_tileRenderer` `0xAD7740`).
  `cTileRenderer_addQuad` (`0x61E800`) appends 4 vertices (FVF `0x244`) to an 8192-vertex VB grouped by
  texture; `cTileRenderer_flush` (`0x61FAB0`) draws one `DrawIndexedPrimitiveVB` per texture group.
  This part is already batched.
- `cWorldView_drawOverheadLabels` (`0x627180`): ALT labels; layout area from `cLabelLayout_init`
  (`0x6620B0`, called with 1024, 768).
- `renderSavePortrait` (`0x4B1450`, thiscall on `0xAACF80`, args path/304/360/0.6f; called from a UI
  window via `0x550550`): clears the back buffer, renders the hero with the same projection constants
  into its center, reads back 304x360 and writes the savegame JPEG (ijl). Not visible on screen.
- View space and screen center: `cWorldView0_render` positions rows relative to `camera - viewSize/2`
  (view size = zoom·1024 x zoom·768, patched to zoom·W x zoom·H), so view space is centered on the screen
  center. The passes then map view to screen as `ftol((v - 512.0f) * zoom) + 512` (and `384`), and cull
  with the inverse `(p - 512.0f) / zoom + 512.0f` against `0..1024` / `-100..818` (tile rows) or
  `-100..888` (`0x62D3C0`). Float and int halves must change together: at zoom 1 the original pairs cancel
  out, patching only the int half shifts everything by `W/2 - 512`. SacredBild patches all of them
  (`resolution_sites.inc`: `ImmHalfW/H`, `MemHalfW/H`, `MemCullH/H2`), including "center - camera" loads
  (`mov reg, 0x200`) in the object passes and the effect code at `0x41B170`, `0x41B200`, `0x41EC20`.
- Draw lists: `cWorldView_renderTileRow` stores object positions relative to the camera with the center
  added (`iso - cam + 0x200`, and for static objects `iso - (cam - 0x200)` via `add reg, -0x200` at
  `0x62B94B`); the object passes convert them back with `(p - 512.0) / zoom + 512.0`. Every one of these
  centers is patched together (missing the `-0x200` form shifted all buildings by `(512 - W/2) / zoom`).
  Most 2D sprites draw with Z off, so their layering against the 3D characters is draw order. Objects with
  flag `0x200` (buildings, portals: the occluders characters walk behind) draw with Z test and Z write on, with a
  depth computed by hand (see "Depth range" below).
- Camera and depth range: view = look-at from eye `0x182EE20` (0, 1200, 600) to target `0x182EE00` (0, 0, 0),
  up +Z (set up by static initializers at `0x811700`/`0x811720`; `0x6284E0` shifts both by the camera). The
  camera looks down at 26.57° from 1341.6 units, so ground `v` world units above the screen center lies at depth
  `1341.6 + 2v`, a point `h` above the ground `2.236h` closer. The world projection's near/far are -600 / 2500
  (`push 0xC4160000` / `0x451C4000` in `0x624E30`, `0x628130`, `0x628310`, `0x6288F0`; `cEngine_ctor` and
  `0x60D760` set an initial -1000 / 1500 one that the world view replaces every frame). That covers a 768 px
  view up to zoom 2 (depths 542..2142). At 2560x1440, zoom 2, ground in the top ~164 px lies past the far plane:
  3D models there lost their lower parts (the far side) or vanished. `0x6288A0` (thiscall (screen y, height))
  gives the Z-tested sprites their depth with the same constants by hand:
  `(2 * (200 - y * 400/768) * zoom - h / sin(26.57°) + view[+0x970AC]) / 3100 + 0.002`, where 200 is the 384 px
  center in world units and `+0x970AC` = camera distance + 600 (`cWorldView_updateViewMetrics`). With the screen
  center at H/2 that put the sprites ~350·zoom units in front of 3D models at the same spot. SacredBild scales the
  range around the camera distance with `H/768` (1440: -2299..3514) and patches the sprite constants to match
  (center `200·H/768`, `-near`, `1/(far - near)`, bias `0.002·768/H`). The model visibility test `0x4057D0`
  only checks x/y.
- `cWorldView_screenToWorld` (`0x629F30`, vtable slot 6): `(mouse - backbuffer/2) * zoom + camera`, uses
  the real back-buffer size.
- Ground layers: `renderTileRow` draws each tile's base through the quad batcher (`+0x86890`, flush
  `0x629340`) and copies tiles with blend layers into an array at `+0xB64` (0x90 bytes each, list heads at
  `+0x3E3C4`, count `+0x3FF1C`, capped at `0x6D5`); `cWorldView_drawTileLayers` (`0x62D3C0`) draws them after
  all rows. A zoomed-out high-resolution view overflows the cap (later rows lose their blends: staircase
  edges), so SacredBild flushes batcher + layers between rows when the array is nearly full.
- Row walk: only the 3x3 sectors around the camera are loaded (64x64 tiles each; `view+0x970B0` 9 sector
  pointers, tile table at `+0x6C`, 32-byte tiles, tile index = row·64 + column, sector = row·3 + column).
  `cWorldView_initRowWalk` (`0x6328C0`, thiscall (pos {+4 x, +8 y}, map data)) snaps the view's top-left corner
  (camera `0xAD7998`/`0xAD799C` minus view/2 and margins) to the tile lattice and finds its sector and tile; the
  even row position lives at `+0x970D4` (x, y float, tile, sector, steps int16), the odd one (+48, +24: one tile
  column further) at `+0x970E4`. `cWorldView0_render` calls `renderTileRow` for both, then steps each one tile row
  down (tile + 0x41, y + 48; at the sector's last row sector + 3, last column + 1, corner + 4). Inside the row
  (`0x62D242`) a tile step right is tile - 0x3F, x + 96, crossing to sector - 3 / - 2 / + 1. Row length is
  `+0x96AC8` (view width / 96 + 12); ground is drawn only for columns `+0x96AB8` - 1 .. length - `+0x96ABC` + 1
  (both 6), the rest are margins for objects. The walked area is the view plus 6 tiles left, 5 rows up and 19 rows
  down; the original view always lies inside the loaded sectors. A large zoomed-out view does not near a sector
  edge (at 2560x1440 zoom 2 the visible part still fits, the margins do not): the corner then is in no sector,
  `initRowWalk` stores sector -1 with a tile index computed from the offset tables at index -1, the steps turn
  that into a valid sector with a negative tile index, and `renderTileRow` (which checks the index only against
  `> 0xFFF`) read before the tile table: crash at `0x62B093` (e.g. leaving a town to the north). Rows leaving the
  loaded sectors to the right or bottom stepped into wrong sectors. SacredBild anchors each frame's walk in the
  192x192 loaded tiles (the game's corner, or a lookup next to the camera stepped back in whole tiles) and runs
  each row only over its loaded part, shifting x, tile, sector, length and the ground columns. `0x62D700` is an
  unreferenced copy of the row walk.
- Input: mouse events (`0x89704C` down, `0x899248` up; x/y at +8/+0xC) carry the UI-space cursor.
  `cEngine_receiveEvent` (`0x618090`) hands them to the UI manager first (UI coordinates), then to the world
  mouse handler `0x6172C0` (thiscall (event, flag)), which picks with them; SacredBild converts the event to
  screen pixels for that call only.
- Hold-to-move: after 0.5 s of holding the button (`0x6172C0`, timer `0xAD6F1C`) the hero walks toward
  `cursor - (512, 384)` turned into an iso direction (`0x4FB7E0`, `lea reg, [mouse + 2*off - 0x200]`). That
  read is redirected to screen pixels, so the center is patched to W/2, H/2.
- Move orders: the mouse handler and the per-frame hold-to-walk (`0x60F130`) build a 0x44-byte order (vtable
  `0x89095C`, `+4` type 4, `+0x14` mode 2 walk / 0 stop, `+0x18/+0x1C` world target from the view's screenToWorld,
  `+0x20` 1 = follow the cursor) and hand it to `cEngine_sendOrder` (`0x617030`, thiscall on the engine (creature,
  order), `ret 8`), which snaps a walk target to a walkable cell (`0x616AD0`) and calls the creature's vtable `+0x18`.
  The move executor (`0x4F4770`, "EiMove") walks a follow order straight at the cursor through `0x4FB7E0`, but for
  a player only while cMouse `+0` bit `0x100` (left button held, set and cleared only by the window procedure) is
  set (`0x4F681A`); otherwise it ends after the next waypoint. Hold-to-walk re-sends the order only while the hero
  is still moving, so a walk something blocked never restarts while the button stays down. SacredBild's stick walk
  (`src/game/hero_move.*`) sends the follow order itself and sets that bit, without the click's path-finding start.
- Taskbar slots (`cUI_Manager +0x80`, setup `0x6E4DF0`, relayout `0x6E01F0`): weapon slots (keys 1-5) at `+0x164`
  and combat art slots (6-0) at `+0x178` (cUI_Static* each, 64x64, taskbar-relative, slots past the hero's slot count
  hidden; the weapon slots move with the count), potion buttons (Space Q W E R) embedded at `+0x258 + i * 0xBC`
  (greyed copies at `+0x604`), absolute (435,667) (465,653) (497,649) (530,653) (560,667) 32x32, drawn only with the
  SHOWPOTIONS option. Keys: the taskbar's receiveEvent `0x6E1BC0` ('1'-'5' `0x6E1690`, '6'-'0' `0x6E1A20`); potions
  through `usePotion` `0x612FA0(type, toHirelings)` from the key switch at `0x6186EE` (B: healing to the hirelings).
- A ground quad (`renderTileRow`, `0x62B0xx`): tile `+0` definition index into the map data's tile definitions
  (`*(view+4) + 0x258`, 0x40 bytes each: `+0x20` texture handle, `+0x24` uint16 cell), `+0xC` first blend layer
  record, `+0x10..0x13` int8 corner heights and `+0x14..0x17` corner light (gray, alpha 255), both in the order left,
  top, right, bottom. Vertices (left, top, bottom, right) at row x/y (view units) plus (-48.2, 0), (0, -24.2),
  (0, 24.2), (48.2, 0), minus the height, all times 1/zoom (`cWorldView_updateViewMetrics` stores the offsets
  divided by the zoom at `+0xA70`); z 0.9, rhw 1; texture coordinates from the 18-cell table at view `+0x830`
  (4 corners x (u, v) per cell). Tile (r, c) of the 192x192 loaded tiles lies at (48 (c - r), 24 (c + r)) plus one
  offset per frame: a row's x grows by 96 per tile, rows step 48 down, odd rows start (48, 24) further. Lighting
  through `cTileRenderer` instead (`0x620090`, `0x61ED70`) only while the world singleton (`0x417E70`) has
  `+0x10 & 0x20` or `+0x9E0C` set.
- Blend layers (`cWorldView_drawTileLayers`): record (`layerRecordCache`) `+4` = stage 0 definition (low 17 bits)
  and stage 1 definition (high 15 bits, 0 = one texture), `+0xC` next id; a record's cell is its definition index
  % 18. Passes alternate between two-texture records (render flag `0x2000` off, `0x629300`: both textures) and
  one-texture records (`0x2000` on, `0x6292C0`: stage 0 only, stage 1 left as it is), starting with the former; each
  pass draws every chain up to its first record of the other kind. The flag ends on. Render flags: instance
  `0x643110` (cdecl), set `0x643430` (thiscall (flag, on)).
- With `[Render] GroundMesh`, SacredBild skips the quad batcher's add (`0x629180`) and texture (`0x6292C0`) calls
  inside `renderTileRow`, empties the layer list after each row and draws its cached sectors in place of
  `drawTileLayers` (`src/game/ground_mesh.*`).
- Animated water/lava tiles (record type `0x90`/`0xA0`): `renderTileRow` appends 0x98-byte entries at
  `+0x3FF2C` (count `+0x80E3C`) with no bounds check; `cWorldView_drawWaterTiles` (`0x62DD00`) draws them after
  all rows (glow pass, tile pass) and sets the water ambience from their count and average position. Entry 1750
  lands on the count: a large water area fully zoomed out at 2560x1440 overwrote it and crashed in
  `renderTileRow` (`0x62B57C`). SacredBild flushes ground, layers and water tiles between rows before either
  array fills up.
- Device calls from other threads: in game the main thread calls the device itself, ~3-4 times per second:
  `0x6284E0` (zoom: `GetTransform` + `SetTransform`) and the render flag setter `0x643470` (filtering, stage
  states). The proxy therefore keeps a lock (a spinlock: the render thread never pays a kernel wake-up).
- Tile layer records come from `0x635FE0`: a map cache of 0x1000 entries filled from the data file
  (fseek/fread per miss) with an O(n) LRU scan per insert when full. A zoomed-out high-resolution view needs
  more entries and thrashed every frame (~140 ms); the limit is raised to 0x8000. The texture manager budget (`0x65E7A0`, set by `initApp`) is not a limit on modern
  machines (computes to ~4 GB).
- In game the frame only clears Z (`cEngine_renderThreadRun`); the ground has to cover the screen.
  SacredBild clears the target before `cWorldView0_render`.
- Frame limit: before each flip `cEngine_renderThreadRun` calls `0x60A9F0` (cdecl (double, fps)) with
  `push 0x3c` = 60 unless global options `0x182EE3C` have `0x4000`; it spins with `Sleep(0)` until 1/fps passed
  since its last return (QPC in `0xAD6E88`, read nowhere else), plus `Sleep(1)` per turn while `0x182EBEC` is set
  and the singleton at `0x182EBE8` has byte `+0x1C`. `cUI_Manager_runThread` calls it as well (menus, also 60).
  SacredBild replaces it with a high-resolution waitable timer (no spin), `FpsLimit` for the in-game value.
- Fade: `cEngine_renderFadeOverlay` (`0x60E060`) draws a full-screen TL quad while engine flags
  (`+0x54`) have `0x20000` (fade out, then latches `0x80000` = black), `0x40000` (fade in) or `0x80000`.
  `cEngine_setFadeMode` and many script functions toggle them.

## Draw calls and batching

Measured at 1920x1200, zoomed out (2.0): ~9,600 draws per frame, 8,400 stage-0 texture switches, ~350
distinct textures, 21.6 ms of 30 ms world time inside `DrawPrimitive*`. Nearly every draw uses another
texture than the one before, so the game's own batching rarely gets past one quad.

- Ground quad batcher at `cWorldView + 0x86890`: `0x629180` (thiscall (device, 4 vertices)) culls against
  0..1024 / 0..768, appends a quad (FVF `0x244`: XYZRHW, diffuse, two texture coordinate sets) and flushes at
  0x252 indices; `0x629340` flushes with one `DrawIndexedPrimitive` and is called whenever the texture changes
  (`0x6292C0` one texture, `0x629300` two). The detail histogram shows mostly 6-index draws: one quad each.
  The flush looks its texture handles (`+0x5920`, `+0x5924`; 0 = leave stage 1 alone) up with
  `cTextureManager_get` (`0x65ED90`, thiscall on `*0x13E7838` (handle, 0): stamps the texture as used, loads it if
  needed, surface at `+0x14`), stage 0 before stage 1. `BatchGround` replaces the flush: same lookups, then the
  textures and quads go to the batcher in one call (zoomed out ~7,000 flushes per frame, each two `SetTexture` and a
  `DrawIndexedPrimitive` through the proxy before). The batcher appends such a flush straight to the pending batch
  when it fits (`Batcher::drawQuads`: same format, no recorded state change, every textured stage through the atlas
  page the batch already uses), without the generic per-draw work.
- `cTileRenderer` (`0x61E800`/`0x61FAB0`, vertex buffer) is not used in game (no `DrawPrimitiveVB` calls).
- Sprites: `dxDriver7_drawTexturedQuad` and the object passes draw FVF `0x1C4` strips and small indexed lists.
- Render state cache: `0x643430` (thiscall (flag, on)) skips redundant changes of a flag word at device
  wrapper `+4`; `0x643470` applies them. Flags: `1` ZENABLE, `2` CULLMODE CCW/none, `4` ALPHABLENDENABLE,
  `8` SRCBLEND one/srcalpha, `0x10` DESTBLEND one/invsrcalpha, `0x20` LIGHTING, `0x40` ZFUNC greater/lessequal,
  `0x80` SPECULARENABLE, `0x100` ZWRITEENABLE, `0x200` COLORVERTEX, `0x400` FILLMODE solid/wireframe, `0x800`
  linear/point filtering (stages 0+1), `0x1000` stage 0 addressing wrap/mirror, `0x2000`/`0x20000`/`0x40000`
  texture stage setups for ground layers, `0x4000` ALPHATESTENABLE, `0x8000` texture transform (count 2),
  `0x10000` stencil. Sprites toggle alpha test and Z writes around each draw; the values at draw time repeat.
- No `ApplyStateBlock` calls exist; `GetRenderState` is used by `cTileRenderer_flush` and a few others.
- Textures come from `dxDriver7_createSurface` type 1: `DDSCAPS_TEXTURE` + `DDSCAPS2_TEXTUREMANAGE`, mostly
  256x256 ARGB4444, some ARGB8888, no mipmaps or color keys. The texture manager (`0x65ED90`) loads and
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
- 3D characters are about half the frame. Model path: `cCreature` vtable[5] render (`0x599910`, thiscall (device, 0))
  -> `cObject3D_render` (`0x44B230`; only with the Granny model attached, flags `+0x14` bit 26) -> `cObject3D_drawModel`
  (`0x44A9D0`, thiscall (device, model, instance, flags64)). Per creature `cObject3D_drawModel` runs a visibility
  test `0x4057D0` (reads world/view/projection back with `GetTransform`), `cGranny_render` (`0x405C90`: per
  mesh piece `GrannyLockNextRenderingState`, SetTexture, SetTransform(WORLD), `DrawIndexedPrimitiveStrided`
  FVF `0x112`/`0x152`, lighting on) and `cGranny_renderShadow` (`0x406FF0`: the same meshes flattened through
  a shadow matrix, FVF `0x142`, stencil flag `0x10000`). `0x401920` calls `GrannyAdvanceTime` per model
  instance (~10 % of the thread); the skinning inside the lock calls is about as much. Granny is 1.x (2002 API:
  `GrannyLockSequenceForRendering`, `...RenderingState`).
- `GetTransform` through d3dim700/DDrawCompat/driver cost ~5 %: SacredBild answers it from the last
  `SetTransform`.
- `0x640410` (5 % exclusive) is the `std::map` find of the map data's record caches `0x635FE0` (ground layer
  records, render thread) and `0x635E50` (64-byte records, 85 call sites incl. game logic). Only these two
  functions insert/evict; the constructor `0x6335F0` and `0x633E80` (unload) create/clear the maps.
  `RecordIndex` puts a `gtl::flat_hash_map` (id -> node) in front, behind a reader/writer lock; it is dropped
  whenever the map's head or size differs from what the index last saw (eviction, clear, new object).
- `0x66FA00`/`0x66FA10` wrap `WaitForSingleObject`/`ReleaseMutex` on kernel mutexes (~2 %).
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

## Character skinning (granny.dll, Granny 1.x)

granny.dll (image base `0x10000000`) is the same file in the English and German installs; addresses below are
granny.dll's. Sacred uses the 1.x sequence API: `cGranny_render` (`0x405C90`) and the shadow pass (`0x406FF0`) each
call `GrannyLockSequenceForRendering`, then `GrannyLockNextRenderingState` per mesh piece (a 0xCC-byte state: vertex
streams for positions, normals and texture coordinates, the piece's index range, texture cookie, a matrix). At
2560x1440 zoomed out those two `LockNextRenderingState` call sites were 14 % + 5.6 % of the render thread.

- `LockNextRenderingState` -> engine `0x10034200`: on the first state of a sequence lock it deforms the whole mesh
  with `0x1001E660` (thiscall on the mesh (bones, positions out, do positions, normals out, do normals, normalize),
  `ret 0x18`); the following states point into the result. The shadow pass locks the sequence again, so every
  character is deformed twice per frame.
- A rigid path skips the deformation: meshes uploaded through the mesh cookie API (`GrannyLockNextNewMesh`), if
  `0x1001DA00` calls them rigid (one bone binding, no vertex-major lists). Sacred never uploads meshes.
- Mesh: `+0x0C` bone binding count, `+0x14` bindings (0x7C bytes each), `+0x18` vertex count, `+0x1C` bind-pose
  positions (float3), `+0x20` normal count, `+0x24` normals, `+0x4C` vertex-major vertex count, `+0x50`/`+0x54`
  vertex-major lists (int stream: per vertex a count and (binding, float weight) pairs), `+0x5C` duplicates (per
  vertex-major vertex a count and offsets, relative to the vertex, of copies), `+0x60`/`+0x64` per-normal list
  pointers (count, (binding, float weight) pairs).
- Binding: `+0x44` length and `+0x48` bone-major runs (first vertex, count, `count` float weights; used for
  positions and normals), `+0x4C` 3x3 matrix (row-major) and `+0x70` translation, computed by the deform for the
  pose: out = M v + t.
- Deform order: zero the outputs; per binding compute its matrix and add the weighted bone-major runs
  (`0x1001DE00`); then either one rigid bone for every vertex/normal (`0x1001E050`/`0x1001E0E0`, if no vertex-major
  vertices and fewer than 2 bindings) or the vertex-major lists (`0x1001DF00`) plus duplicates and the per-normal
  lists (`0x1001E490`), each overwriting; optionally normalize the normals.
- `cGranny_render` reads about 11 deformed positions per piece for a bounding box.

- Callers of the deform: the rendering path (`0x10034200`, call returning to `0x100343C0`; a second call there is
  the rigid mesh cookie path with no outputs) and `GrannyLockSequenceForRayIntersection` (`0x100359B0`): picking
  reads those positions on the CPU.
- The draw (`cGranny_render`): `DrawIndexedPrimitiveStrided`, triangle list, FVF `0x112` (or `0x152` with a
  per-vertex diffuse stream from the table at `0xA23FC0`), positions and normals pointing at the deform's output
  buffers (the whole mesh, vertex count = the mesh's), texture coordinates at the mesh's static array, the piece's
  indices; `SetTransform(WORLD)` from the state before.

`[Debug] SkinCheck` (`src/game/skin_check.*`) rebuilds per-vertex weights from these lists and compares its own
skinning with Granny's output, with all influences and with at most four per vertex. Measured (66 meshes, 16.5M
vertices): at most 4 influences, at most 56 bones per mesh, normals weighted exactly like their positions, paths
rigid / vertex-major / normal lists; differences of one float step (Granny accumulates in x87 extended precision).

`[Render] GpuSkinning` (`src/game/gpu_skin.*`, `src/ddraw9/device_skin.cpp`, `skin.hlsl`): for the rendering
path's deforms (the model pass wants positions and normals, the shadow pass positions only) the deform runs with both
outputs off, which still computes every binding's matrix; the module keeps those and CPU-skins only the vertices
`cGranny_render` samples for its bounding box (every `count < 12 ? 1 : count < 23 ? 2 : count < 34 ? 3 : count / 11`-th).
The draw that follows is recognized by its position pointer and drawn by the backend from a static vertex buffer
(bind pose, texture coordinates, 4 bones and weights) with a vs_2_0 shader that skins and lights like Direct3D 7's
fixed-function pipeline; otherwise the module skins the whole mesh on the CPU first. The shadow pass
(`cGranny_renderShadow`, `0x406FF0`) locks the sequence with flags `0x21` (positions only) and draws FVF `0x142`:
positions, one constant diffuse color (stride 0), texture coordinates, lighting off, stencil on, its world matrix
flattening the mesh onto the ground (singular: the shader's normal matrix is only computed for lit draws).

### Animation advance (GrannyAdvanceTime)

`GrannyAdvanceTime` (`0x10029710`) -> engine `0x100337A0` -> scene `0x10007080`: adds the elapsed time to the scene
clock, advances every animation control (`0x10007420` -> `0x100016E0` per control, which samples the animation
into the bones' local transforms; accumulated root motion included) and then poses every skeleton in a list
(`0x10007500` -> `0x10008460`). The pose update (thiscall on the skeleton, `ret 8`) runs once per frame per skeleton:
it skips inactive ones (`+0x6C`) and those whose stamp (`+0x78`) equals the global pose counter (`0x1006DC54`),
stamps them, poses the parent skeleton (`+0x74`) first and then each bone (count `+0x10`, 300-byte bone states at
`+0x18`; `0x10021690`: local to world, `0x10021250` then `0x10020990` with the parent bone). The counter is incremented
after the list, so a skeleton posed by the last advance carries counter - 1 (the constructor `0x100084F0` starts it
there). The deform's first argument points at the same skeleton object.

Measured at 2560x1440 zoomed out: ~2.9-3.4 ms per frame on the animation worker, 220-310 skeletons (10,000-12,000
bones) posed per frame of which only 70-120 were drawn. Busy time by function: `0x10021250` 20 %, `0x10020E10`
15 % (accumulating a control into a local transform), `0x10020990` 11 %, small 3x3 helpers the rest.
`[Render] OffscreenPoses` poses skeletons not drawn in the last two frames only every Nth frame; the deform hook
poses a skipped one before it is drawn and stamps it (and parents) back to counter - 1.

## UI (`cUI_Control2` / `cUI_Window2` / `cUI_Manager`)

`cUI_Control2` (vtable `0x897488`): `+0x10` flags (bit 0 visible), `+0x24` x, `+0x28` y, `+0x2C`/`+0x2E`
width/height (int16), `+0x30` name (char[0x20], uninitialized for default-constructed controls),
`+0x50` parent. Render = vtable `+0x14`.

`cUI_Window2` (vtable `0x8971D8`, type descriptor `0x9E0170`) adds a child vector at
`+0x78/+0x7C/+0x80`. `cUI_Window2_addChild` (`0x727890`) only pushes into that vector, it does not
set the parent pointer; most constructors pass the parent to the control's constructor instead (the
inventory's border pieces are created at negative y relative to the window), so children are usually
relative. `cUI_Control2_getAbsoluteRect` (`0x732350`, 154 calls) adds parent offsets when `+0x50` is set, and
most hit tests and tooltips go through it. Window code still draws and tests at fixed 1024x768 positions in
places: the taskbar's center ornament at (512, 710) (`0x6E3F70`, `0x6E3E00`), its hit test (`0x6DFD60`: x 384..640,
y 676..768), the minimap's radar ping at (975, 70) (`0x6D70F0`), the party arrows along the screen edges
(`0x6D01F0`: 8..1008 x 8..752), the megamap's scrolling at x 1023 / y 767 (`0x6C7920`).

Virtual functions (thiscall, vtable byte offsets): `+0x10` receiveEvent(cEvent*) -> bool, `+0x14` render(device),
`+0x1C` isInside(x, y) -> bool, `+0x24` show(bool), `+0x30` update, `+0x34` / `+0x38` ESC / Enter (from
receiveEvent), `+0x44` render2(device, ?, ?) on blacksmith, merchant, net portraits (and menu windows). Mouse
button events: cEventMouseDown (vtable `0x89704C`, type 2) and cEventMouseUp (`0x899248`, type 3), x/y at
`+8`/`+0xC`; there is no mouse move event, hover reads the cMouse. The manager calls `+0x10` of each window in
turn (`cUI_Manager_receiveEvent` `0x757840`), `+0x14`/`+0x44` to draw and `+0x1C` from
`cUI_Manager_isCursorOverUi`.

Construction: `cRect_ctor(x, y, w, h)` (`0x6A57C0`) builds the rect passed by value to
`cUI_Window2_ctor` (`0x7247E0`), which calls `cUI_Control2_ctor(name, x, y, w|h<<16, parent, flags)`
(`0x731B00`). Several derived constructors reposition the window afterwards (inventory -> (0,388),
equipment -> (656,388), mercenaries -> (932,0), ...).

`cUI_Manager` (vtable `0x8977D0`): `cUI_Manager_createGameWindows` (`0x759AF0`) creates the in-game
windows into `+0x80..+0xD8` (taskbar, inventory, equipment, blacksmith, merchant, megamap, overview map,
minimap ("UI_WND_MERC": minimap and party portraits, 932,0 92x676), stats, console, questbook, escape menu,
purchase, master, savegame, options, chest, horse, net info, net portraits, character, cube, trade; rects and
offsets in `Sacred::UiManager`). The shop windows (blacksmith, merchant, master, chest, cube, trade) are 640 wide
at 0,0, next to the inventory at 0,388. `cUI_Manager_render` (`0x758ED0`, arg: device) draws the
cinematic letterbox bars (float immediates 1024/768) and all windows. Full-screen menus are separate
`cUI_Window2`s with rect (0,0,1024,768) or (0,0,1023,767). In game (mode bits `0x04` and `0x40`) a visible savegame (`+0xAC`),
options (`+0xBC`) or character (`+0xD0`) window is drawn alone; the megamap (`+0x94`) alone with the tutorial
hints and popups; otherwise all HUD windows. The escape menu (`+0xB0`) and popups follow in every case.

Input: `getClientCursorPos` (`0x66E280`, cdecl `(HWND, POINT*)`) is the cursor read for the game; the
window procedure is `sacredWndProc` (`0x812BF0`, class "Sacred" registered by `0x813B90`). It calls
`getClientCursorPos` for WM_MOUSEMOVE (then `cMouse_setPosition`) and the button messages, whose events
carry that position. `cEngine_scrollViewAndWarpCursor` (`0x6105B0`) also uses GetCursorPos/SetCursorPos
(screen pixels) to keep the cursor on its world spot while the view scrolls. (`0x664690` creates the
debug "LogWindow", not the game window.) `cMouse_instance` (`0x654F60`) holds x/y at `+4/+8` and optional clamp
bounds at `+0x14..+0x20`; `cMouse_getX`/`getY` (`0x655850`/`0x655860`) are thiscall getters,
`cMouse_renderCursor` (`0x655450`, `(device, flag)`) draws the cursor. UI code reads `+4/+8` directly or
through events. World code reads the mouse in `0x611E70`, `0x6172C0` (getters) and `0x4FB7E0`,
`0x60F130`, `0x610380`, `0x611E70`, `0x627180` (instance + direct field reads).
`cInventoryEntry_render` (`0x5DC430`) and the getter callers in `0x6B6690`, `0x6DA320`, `0x6DBF50`,
`0x6DD970` are UI. `cEngine_updateWorldCursor` (`0x611E70`, render thread, before the UI) reads the mouse
once and passes it both to `cUI_Manager_isCursorOverUi` (`0x75AA90`, UI coordinates; if true no world
pick happens) and to the world pick, so SacredBild converts back for that one call (`0x611EE3`).

Frame loops: `cUI_Manager_runThread` (`0x757060`, menus): `Clear(target|z)`, `beginScene`,
`getClientCursorPos`, `cUI_Manager_render`, `cMouse_renderCursor`, `endScene`, `flip`; while loading it
calls `dxDriver7_drawLoadingScreen` via `0x759720` instead. `cEngine_renderThreadRun` (`0x60E350`,
in-game): world, overlays, `cUI_Manager_render`, cursor. `playVideo` (`0x6A0EA0`, thiscall, 5 args):
DirectShow video into a 1024x512 texture drawn as a (0,0)-(1024,768) TL quad with its own frame loop.

Popups (tooltips, hints; class vtable `0x896B3C`, constructor `0x6E67C0`) live in the manager's vector at
`+0x124/+0x128` and are drawn by `0x758DC0` after the windows. Callers set the popup's x/y and then its text with
`0x6E6EA0` (string) or `0x6E6FB0` (text id), which flags a new layout (`+0x154` bit 0x40); the popup's render
(`0x6E76C0`) then lays it out (`0x6E7AF0`: centered on x/y, clamped into 16..1008 x 16..752, or centered on the
screen). Show helpers: `0x75B390` / `0x75B430` (x, y, text, ...); the equipment window sets x/y itself
(`0x6B7700`). `cUI_Manager_showHelp` (`0x75B4F0`, thiscall (device, short screen), from `cUI_Manager_render` every
frame) is the help screen: with manager flag `0x200` (H key) it puts the help popups (indices: uint16 array at
manager `+0x56`) at fixed 1024x768 positions (tables at `0x17EE9D8..0x17EEBB8`, x, y, w|h<<16; text ids from
`0x9EC534`) and sets their texts, flags `0x201`; without it the hints "[H] shows or hides the help screen" and
"[TAB] for the overview map" at (425, 100) and (425, 170) (manager flags `0x400`, `0x800`). The screen
follows the open windows: 1 none (general hints), 2 inventory (entries next to the stats window, over inventory,
equipment and taskbar), 3 blacksmith, 4 combo master (`0x6D1440`), 5 merchant, 6 world map, 7 rune exchange at the
master (`0x6D1420`). Every screen's first entry is "[H] shows the help screen".

The cMouse (`cMouse_instance` `0x654F60`, a 0x70-byte singleton behind `0xCDDB5C`): `+4/+8` position, `+0xC/+0x10`
the drawn position (copied and clamped to `+0x14..+0x20` by `0x6554E0`), `+0x64` cursor image, `+0x68` the item
held by the cursor (`0x655550` get, `0x655580` set, `0x6556F0` clear). Reads of the position: 89 sites, 17 in
world code, the other 72 in the UI (55 `cMouse_instance()` followed by reads of +4/+8 only, 6 `getX`, 6 `getY`,
5 `getCursorPos` `0x655810`), two of them outside the UI code range (`cInventoryEntry` `0x5DC310`, `0x5DC430`).

### SacredBild UI canvas

Per-window re-anchoring does not work: children are absolute, many renderers draw at absolute
1024x768 positions, and windows are "hidden" by parking them off-screen. Instead the whole UI stays in
1024x768 space and is drawn into a centered canvas (`src/game/ui_canvas.*`):

- UI scopes: `cUI_Manager_render`, `playVideo` (confined to the canvas) and `cMouse_renderCursor`
  (overlay: mapped, but not culled or clipped, so the cursor also shows beside the canvas; its 3D draws, the
  carried item, get the whole screen as viewport and a projection scaled and shifted in clip space so that the
  game's 1024x768 viewport still lands on the canvas).
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
- UI images are rects in 256x256 sheets: records of 0x54 bytes from `0x9ECEB0` (`+0` id, `+4` type, `+8` TGA
  name, `+0x28` texture, `+0x2C..+0x38` x0, y0, x1, y1 in pixels at load, `+0x4C`/`+0x50` width/height).
  `0x761580` makes x1/y1 exclusive and turns them into `u0 = x0/256`, `u1 = (x1 + 0.5)/256` (same for v): drawn
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
parent, set by the taskbar setup `0x6E4DF0`) lies on the stats window's close button and is covered by it while
the window is open; it is moved within the taskbar's (unconfined) frame by the offset between the two frames.
The taskbar draws it (`+0x14`), highlights it while flag 0x400000 is set (`0x732220`, non-virtual, reads its rect)
and hit-tests it through `+0x1C` / `+0x10`. The item held by the cursor is drawn by every window that takes items
(`cInventoryEntry_render` `0x5DC430` with (device, 1), at `getCursorPos`); it runs unconfined. The effects list
behind the portrait's "+" is a popup at (16, 16) with the clamp flag (`0x6D7B80`): the game's way of saying top-left
corner. Popups are drawn unconfined, and their layout (`0x6E7AF0`, hooked) runs without the game's clamp and
centering (the flags' bits are cleared for the original, the children are laid out afterwards): a popup the game
would push against an edge of its 1024x768 screen goes against that edge of the real screen (in the popup's
frame), centered ones go to the screen center. The help screen sets all its texts in the canvas; after
`cUI_Manager_showHelp` each entry that explains a window gets that window's frame (a table per screen and entry), the
inventory screen's "[H]" hint (20, 80) the screen's top-left corner, general hints and the world map's stay in the
canvas. Untextured translucent black quads over the whole 1024x768 screen
are drawn over the whole screen: the escape menu's background (`0x6BB390`, 0x6F000000) and the dimmed background
of message boxes (windows with flag 0x800, `cUI_Control2_render`, 0x9F000000). The stats window's close button is an embedded control at `+0x210` named `UI_STAT_BTN_BASE`
(stats-relative 242,10, parent set); the game flags controls of that name with 0x20000 on level-up (`0x57F033`), and
the stats window's show (`0x6A5FA0`) messages them as well. `[Debug] UiTrace` logs a frame's UI draws with their frame
and the calling code (a stack scan for return addresses into sacred.exe) for finding the rest.
Known gap: the party arrows stay in the canvas. The cinematic letterbox bars (`cUI_Manager_render` with manager
flag `0x10`: untextured opaque black FVF `0x1C4` strips, x 0..1024, y 0..h and 768-h..768, h growing to 64) are
moved against the screen's top and bottom edge across its width (`DeviceProxy::drawScreenBar`).

Around the canvas (`[UI] Backdrop`, `src/ddraw9/backdrop.*`): a menu frame's canvas is blurred at the flip (halved
with bilinear StretchRects down to ~32 texels wide) and drawn darkened, scaled to cover the screen, into the bars
of the next menu frame before `cUI_Manager_render`; the loading screen does both right after its blit; with an
in-game full-screen window open, the world drawn beside it is darkened instead of cleared.

World map (megamap, `cUI_Manager +0x94`, vtable `0x896714`: render `0x6C9F60`, receiveEvent `0x6C8140`, show
`0x6C7BB0`): a 1024x768 view scrolled over the map image. Scroll at `+0xC016C`/`+0xC0170`, clamped to the map size
(`+0xC0160`/`+0xC0162`) minus 0x400/0x300; edge scrolling (4 px per frame) when the cursor is at x < 1 / > 0x3FE,
y < 1 / > 0x2FE outside the control rect at `+0xC0538` (cursor shapes per edge, `0x6C7920`); dragging with the
button held (`+0xC0154` bit 8 and the previous cursor at `0x17EA118`). The map pieces are drawn by `0x6C9C60`,
`0x6C8A40`, `0x6C8800`, `0x6C9330`, `0x6C9520`; then a 10-vertex black FVF `0x1C4` strip masks everything outside
(70, 70)..(954, 698), the frame's children (`+0xC04BC`, `+0xC0440`, `+0xC0544`, `+0xC0600`) draw on top, and
marker popups are centered on (0x200, 0x180). A map filling the screen would need those 1024x768 constants, the
mask and the children's layout moved to the screen's size.

## Things that read the back buffer

| Function | Behavior at > 1024x768 | SacredBild |
|---|---|---|
| `cEngine_captureInternal` (`0x613670`), savegame thumbnail | copies `desc.dwHeight` rows into a 1024x768 buffer: **heap overflow** | `lockBack` hook hands it the centered 1024x768 (return address `0x6136AB`) |
| `renderSavePortrait` | size-aware (centered region) | projection constants patched; runs outside the UI canvas |
| `captureScreenshot` (`0x6487D0`) / `writeTga1024x768` (`0x6482C0`) | screenshots of the top-left 1024x768, sheared at 32 bits (`writeTga` `0x6604B0` ignores the pitch), JPEG copy assumes 1024x768 too | mode -1 replaced: whole back buffer as PNG/JPEG through WIC (`src/game/screenshot.*`) |
| `captureScreenshot` modes >= 0 / < -1 (frame sequence into the mapped file `vidobj`, then `cap%04d.tga`) | 3 MB frames copied without pitch: garbled | unreachable: every `dxDriver7_startCapture` (`0x648660`) caller asks for 1 frame; left as is |
| `debugCaptureCharacter` (`0x6F54D0`) | developer tool, reads fixed offsets inside the surface | ignored |

## Hard-coded 1024x768 elsewhere

UI functions comparing against 1023/767 or using 768 offsets (`0x6C7800`, `0x6C7920` megamap
scrolling, `0x6C9C60`, `0x6CCBD0`, `0x6E7AF0` taskbar, `0x710270`, `0x75A480`, `0x75AF90`) and windows
positioned at runtime (`0x75B390`, `0x75B430`) are correct inside the UI canvas and need no patches.

## Networking: LAN games

Game traffic goes through Ascaron's `tincat2.dll` (TCP and UDP sockets, imported via WSOCK32). Finding LAN
games is the game's own code on top of Winsock:

- **Hosting**: creating a game (`cUI_NetLan`, action 5) writes `GameServer.cfg` (`writeGameServerCfg`
  `0x7EEE70`: `NETWORK_IP_ADDRESS`, `NETWORK_PORT_LISTEN`, session name, ...) and starts `GameServer.exe` with
  `CreateProcessA` (`0x7D8C81`, no window).
- **gameserver.exe** (Ghidra `/GameServer.exe (GOG)`, timestamp `0x452F8580`; the German one, `0x451BBDBF`, has the
  same code at the same addresses): app object `*0x634238`, network object
  at app `+0x40`. `cNetServer_initNetwork` (`0x4E8AB0`) opens the ping socket (`+0x1A8`): UDP, `SO_BROADCAST`,
  `connect()`ed to `255.255.255.255:<NETWORK_PORT_LISTEN>`. `cNetServer_sendAnnouncement` (`0x4E99A0`, called
  periodically and when players join or leave) fills the plain announcement at `+0x596C`, runs it through
  `CompressMemory` (`0x4918B0`) and `send()`s it — the exe's only `send` call.
- **Announcement** (0xAE bytes): `+0` u16 version check (from `TinCat_GetBuildNumber`), `+2` u16 gameserver
  TCP port, `+4` u32 IPv4 address in host byte order, `+8` u32 flags, `+0xC` u8 players, `+0xD` u8 max players,
  `+0xE` wchar_t name[80]. The address is `0x6341B8`: the first address `detectLocalIps` (`0x495350`) found, or
  `NETWORK_IP_ADDRESS` if it is one of them. `detectLocalIps` reads the IP address table through SNMP
  (`inetmib1.dll`) and keeps at most **three** addresses (skipping 0, 127.x and x.x.x.254/255); without SNMP it
  falls back to `gethostbyname`, one address. sacred.exe has the same code (`0x804A60`, table at `0x182ECF0`).
- **Wire format** (`CompressMemory` / `UncompressMemory` `0x801580`): u32 header = payload size, bit 27 set if
  not compressed. Payloads under 0x50 bytes are XOR-chained (`plain = (prevCipher + 0xB5D6C7A3) ^ cipher`,
  seeded with the size, the last size % 4 bytes raw), larger ones zlib. The receiver accepts both for any size.
- **LAN list** (`cGCclass`, `*0x182EBF0`): `cGCclass_initNetwork` (`0x7D2BD0`) binds a UDP socket (`+0x14`,
  `SO_REUSEADDR`) to `INADDR_ANY:NETWORK_PORT_LISTEN` (2005). `cGCclass_pollLanGames` (`0x7D35F0`) drains it
  with `select` / `__WSAFDIsSet` / `recvfrom` (their only callers), accepts packets that decode to 0xAE bytes,
  keys games by (address, name) and drops a game after 5 s without an announcement. The sender address is
  ignored: `cGCclass_joinLanGame` (`0x7D3D00`) connects to the address in the announcement.

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
(`0x7D8560`) and configures a `TinCatValues` object (vtable `0x1003CE60`). Its fields match the keys of TinCat's
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
2 = LAN, 0 = not set; `setNetSpeedMode` `0x7EDC20` stores it and sends message 0x21). It does three things:
- client: TCP_NODELAY on the TinCat connection for LAN only, decided once at `cGCclass_initNetwork`
  (`cmp ebx, 2; sete al` at `0x7D310E`; SacredBild's `[Net] NoDelay` makes it 1 in both modes);
- it travels in the first-contact message (`cGCclass +0x40064`, copied before `cGCclass_joinLanGame`); the
  server keeps it in the player record (`net + 0x1D4 + slot * 0x518`) and updates it on message 0x21;
- server: object simulation is distributed to clients. `setObjectOwner` (`0x4D6950`) sets a creature's owner
  slot (`+0x39`, recursively for its children) and sends message 0x171. When a creature needs a new owner, the
  main loop (`0x4C5082`) walks the players that see it and skips ISDN players
  (`cNetServer_isPlayerIsdn` `0x4DCDC0`) for creatures with flag `0x20` at `+0x200` (ambient NPCs). The player
  list draws ISDN players in another color.

Community reports: joining in LAN mode fails over the internet ("IP: Cannot Connect!", connect time-outs),
joining in MODEM/ISDN works and switching to LAN afterwards is fine; ISDN hides most ambient NPCs. Cause:
`KRNL_SendAsyncLogonRequest` (tincat2 `0x10004650`) starts a non-blocking connect (`NET_Async_Connect`
`0x1000D1C0`) and, with `drv_disable_nagle`, immediately calls `NET_SetTCPNodelay` (`0x1000D130`). Windows refuses
TCP_NODELAY with WSAEINVAL while the handshake is under way, and TinCat fails the join with the default code -20
(its table text: "Kernel: Cannot allocate logdata message"). On a LAN the handshake is usually done by then.
SacredBild hooks tincat2's WSOCK32 imports and sets the option before the first send instead (`src/net/connection.cpp`). Switching after
the join keeps TCP_NODELAY off (TinCat is not re-initialised) but makes the player an owner candidate.

**TinCat's sockets** (all through tincat2's WSOCK32 imports, by ordinal): `NET_Connect` `0x1000D9E0` (blocking
connect, then `select` with a time-out; used by the synchronous logon `0x10004380`), `NET_Async_Connect`
`0x1000D1C0` (non-blocking, `NET_SetNonblocking` `0x1000D010`), polled by `KRNL_ProcessPendingConnects`
`0x10004790` through `NET_CheckConnected` `0x1000DBC0` (`select` read/write/except with time-out 0; once writable:
`NET_SetBlocking` `0x1000D0A0`, `getsockname` `0x1000D470`). Server: `NET_Init` `0x1000CD90` (socket,
`SO_REUSEADDR`, bind, `listen(5)`), the server thread `0x100026E0` alternates `NETDRV_ServerLookForFlags`
`0x10001850` (sleeps `drv_serverloop_sleep`) and `NETDRV_ServerLookForNewConnection` `0x10001E40` ->
`NET_WaitForNewConnection` `0x1000CF10` (`select` on the listening socket, `accept`, `TCP_NODELAY`); the listening
socket is closed while the game is full (`NETDRV_CheckAcceptConnections` `0x10001AD0`). One TCP connection per
player, **blocking** once connected: a reader thread blocks in `recv` (`0x1000CC50` / loop `0x1000CB50`), a writer
thread (`0x10002360`) waits on an event and sends queued messages (`0x1000CBC0` / loop `0x1000CAF0`). Message
framing: a 0x1C-byte header (+0 magic `0xDABAFBEF`, +0x14 payload length, +0x18 payload checksum `0x1000EA10`) and
the payload, sent with **two** `send()` calls. Close: `shutdown(SD_BOTH)` + `closesocket` (`0x1000CD60`).
SacredBild uses this to send each message with one `send()`, and for `[Net] Udp` (`src/net/tincat_shim.cpp`).

Server time-outs (`cNetServer_watchdogThread` `0x4DBCB0`, 5 ms loop, times from `0x6340C8`; player record =
`net + 0x1B0 + slot * 0x518`): first contact must arrive within **5 s** of the connection (`+0x60`, checked every
second; SacredBild makes that `[Net] JoinTimeout`, 30 s by default); loading may take 4 min (`+0x74` set, `+0x64` last receive); in game, three 30 s ticks without any data
(`+0x6C..+0x6E`, cleared by `cNetServer_onReceive`) kick the player; idle warning at 14 min, kick at 15 min
(`+0x68` last activity, not the host).

## Input, the world pick and the options window (controller)

- No DirectInput: keys and buttons come through `sacredWndProc` (`0x812BF0`) and 48 `GetAsyncKeyState` polls (Shift,
  Ctrl, Alt, `VK_LBUTTON` for hold-to-move in `0x6172C0`, the zoom keys in `cEngine_handleZoomInput`). `WM_KEYDOWN`
  calls `GetKeyboardState` and `ToAscii` / `ToUnicode` and posts a key event (vtable `0x897568`) through
  `0x808E50` / `0x8092F0`; Shift / Ctrl / Alt are tracked in `0x182EE70` from their own key messages; Ctrl+B and
  Print Screen take screenshots right there. Mouse button messages take the position from `getClientCursorPos`, not
  from lParam. Keys are fixed (Quickstart.pdf): Esc, Tab, I, F, C, L, M, O, S, H, N, P, A (collect all), F8 / F9,
  numpad +/-, Space Q W E R (potions), B (heal hirelings), 1-5 weapon slots, 6-0 select the active combat art (the
  right mouse button uses it), Ctrl+click attacks in place, Shift+click on the ground walks.
- World pick `0x626C50` (thiscall on the world view, `(x, y, excludeId, int32 outRect[3])` -> id): the view's pick
  list is a std::vector of 0x1C-byte entries at `+0x96B24` / `+0x96B28` (id, x, y, int16 w, h, in screen pixels),
  rebuilt by the renderer each frame and guarded by the CRITICAL_SECTION at `+0x96B0C`; more than 1000 entries give
  0. Hits under the point are grouped by category (object vtable `+0x24` (1, 0), 0..3, ranked 0 1 2 3, with Alt
  2 1 0 3: 2 are items) and the nearest center wins. Callers: `cEngine_updateWorldCursor` (stores the hovered id in
  cMouse `+0x6C`, picks the cursor image) and twice the world mouse handler `0x6172C0`. The world view is
  `engine +8 + 4 * (uint16 at engine +0x48)`.
- Objects: `*0xAD5C40` is the object manager; `0x5FE000` thiscall (id) -> object (it logs and fixes a stale id at
  object `+0xC`); `0x603E30` thiscall () -> the local hero, already `__RTDynamicCast` (`0x84A961`) from cObject
  (`0x8EB648`) to cCreature (`0x8EB660`), via the player info singleton `0x7D84A0` (`0x182EBE8`, hero id at `+0x14`).
  `0x548F60` thiscall on a creature (target) -> 1 if the target is alive (`+0xFC` != 9, `+0x150` != 6, `+0x4D8`)
  and hostile (relation of the ids at `+0xC`); the cursor's attack symbol uses it.
- Options window `cUI_Options` (vtable `0x897078`, constructor `0x716D30`, controls built in `0x718CA0`): show
  (`+0x24`, `0x717040`) fills the controls from the options object `0x182EE78` (`0x718660`), OK (`+0x15C`, Cancel
  `+0x158`) stores them under their settings.cfg keys and applies them (`0x717C20`). Check boxes and radio buttons
  are bit 0x10 of the control flags (`0x732550` sets, `0x7325C0` clears; both call show on visibility changes);
  sliders: `0x753430` fastcall get (0..count-1, count at `+0x88`), `0x7533B0` thiscall set. Controls: see
  `Sacred::Options` in `src/game/sacred_addr.h` (the fourth slider is MINIMAP_ALPHA). The main menu uses the same
  class. Labels are text keys (`UI_CFG_*`) looked up by `0x672740`: id = hash of the key (`0x80EAA0`: upper case,
  `id = (c + id * 0x71) % 0x3B9AC9F7` in signed 32-bit arithmetic, then `& 0x7FFFFFFF`), searched in the text table.
- UI manager windows: `createGameWindows` (`0x759AF0`) fills `+0x80` taskbar, `+0x84` inventory, `+0x88` equipment,
  `+0x8C` blacksmith, `+0x90` merchant, `+0x94` megamap, `+0x98` overview map (Tab), `+0x9C` minimap, `+0xA0` stats,
  `+0xA4` console, `+0xA8` questbook, `+0xB0` escape menu, `+0xB4` purchase, `+0xB8` master, `+0xAC` savegame,
  `+0xBC` options, `+0xC0` chest, `+0xC4` horse, `+0xCC` net portraits, `+0xD0` character, `+0xD4` cube, `+0xD8`
  trade. The menus (flags `0x01`) are the windows at `+0x130` .. `+0x140`; `+0x144` is drawn over both when set.
  Window children: std::vector of controls at `+0x78` / `+0x7C`. Hit tests (`+0x1C`, `0x732420`) add the parents'
  positions (`+0x50`) like `getAbsoluteRect` (`0x732350`).
- Game menu `cUI_EscMenu` (vtable `0x8964DC`, rect 380,284 264x200): `0x6BBBD0` builds its five entries as
  `cUI_StaticText64` (`0x753B90`, 0x9C bytes, rect 0,y,width,0x20 at y = 8, 0x2C, 0x50, 0x74, 0x98; parent the menu)
  in a vector at `+0x158` / `+0x15C`, not among the children; receiveEvent `0x6BBA80` and render `0x6BB390` walk it.
- Start menu `cUI_MainMenu` (`+0x130`, vtable `0x897004`, constructor `0x7119B0`): `0x713670` builds its entries as
  `cUI_StaticText64FX` (`0x754490`, 0x8C bytes, 384,y 256x30, 32 apart) in a vector per screen, `+0x158` main,
  `+0x164` multiplayer, `+0x170` extras (12 bytes apart), not among the children; the screen shown is `+0x154` (3:
  credits), and receiveEvent `0x7125B0` / render `0x7128D0` walk that screen's vector.
- Savegame window `cUI_Savegame` (in game `+0xAC`: saving; menus `+0x13C`: loading; vtable `0x8970E4`, constructor
  `0x71BD40`, layout `0x71EFE0`, show `0x71D720`, receiveEvent `0x71D8A0`, render `0x71E4F0`, commands `0x71CC90`):
  mode `+0x75C` (1 saving, 2 loading, 3-5 asking with the mode at `+0x760`). Four rows with hit rects at `+0x72C`
  (0xC apart): row 0 the quicksave `GAME00.PAK` (entry at `+0x168`), rows 1-3 the list's entries (vector of 0x310
  bytes at `+0x15C`, `+4` file name; filled by `0x71EAA0`, sorted by file name) from the first listed one (`0x71C2B0`:
  slider `+0x478` value, capped at count - 3 loading / count - 2 saving). Selected row `+0x156` (`0x71C360`
  selects). Buttons are members (`+0x764` load / save, `+0x820`, `+0x8DC` delete, `+0x998` back). The file header
  (`0x71E600`) holds the save time at `+0x5C` (year, month, day, day of week, hour, minute, second, ms; int32 each).
- Message box `cUI_BusyDlg` (`+0x144`, vtable `0x897134`, 0x644 bytes from `0x756A90`): its buttons are members
  (`0x72A8F0`), laid out per kind of box by `0x722500` (OK left of Cancel). Esc (`+0x34`, `0x721320`) only cancels
  kinds 5 and 6 (`+0x154`).
- Log book `cUI_Diary` (`+0xA8`, vtable `0x8963E4`, receiveEvent `0x6AED40`): four tab buttons at `+0x538` (0x7C
  apart, parent set), the selected one at `+0x7A4` (uint16, 4 until one is picked, `0x6AF0E0` selects), one `cUI_Book`
  per tab in the vector at `+0x7A8` that gets the events. `cUI_Book` (0xA24 bytes, receiveEvent `0x6B3940`): seven
  tabs down the left at `+0x168` (0xBC apart, selected `+0x15E`, `0x6B1880`), page buttons left previous / next
  `+0x68C` / `+0x748`, right `+0x804` / `+0x8C0` (`0x6B0120` / `0x6AFFD0` with side 0 / 1; pages `+0x162` / `+0x160`).
  The left page (`0x6B0240`: book +0x60,+0x20, 0x11C x 0x1C0) lists entries: `0x6B3640` thiscall (x, y, uint16*)
  hit-tests them by measuring their texts; per tab a vector at `+0x97C + 0xC * tab` of 0x1C-byte pages, `+4` the
  selected entry, whose text the right page shows.
- Inventory `cUI_Inventory3` (`+0x84`, vtable `0x896668`, receiveEvent `0x6C61C0`): its pages (backpack, combat
  arts, combos) are a `cUI_TabControl` (vtable `0x897220`) at `[[inventory +0x154] +0x78]`; event 5 / `0x11` with
  1..3 selects one (`0x729940`). `cUI_TabControl`: vector of 0x18-byte pages at `+0x78` / `+0x7C` (`+0x10` the tab
  button, `+0x14` the page window), active page `+0x84` (uint16), `setActivePage` `0x729260`; receiveEvent `0x728350`
  hit-tests the tabs at their absolute rect moved 4 pixels down, visible or not.
- The cursor (`cMouse_renderCursor` `0x655450`, image `0x6548A0`): while it carries an item (cursor image flag 2)
  it draws the item's model in 3D at the cursor, projected into the UI's 1024x768 viewport.

## Language files

- **Language**: `g_language` (`0x17E7D34`) indexes `g_languageCodes` (`0x899394`, `char[16]` each: `US DE FR SP IT
  PL HU JP VC RU CZ`). `WinMain` (`0x817B30`) sets it to 1 (DE), `loadSettings` (`0x813CB0`) to the code that equals
  `LANGUAGE` byte for byte (else it stays), then a code in lower case on the command line wins. Besides the files it
  switches PL keyboard handling (`sacredWndProc`), JP/VC IME and line breaking (fonts, `0x64EC54` ...), hides the
  loading screen's status text for SP, and goes into the hero (`AMH`, `0x604690`) and savegame (`AMS`, `0x619BF0`)
  headers; the savegame loader (`0x61B6F0`) only logs it. Speech playback (`0x693FE0`) is passed it and ignores it.
- **Text**: `WinMain` formats `.\SCRIPTS\<code>\global.res` and calls `cTextTable_load` (`0x80E680`, thiscall on
  `g_textTable` `0x182ED50`), which in the GOG builds ignores the path: it loads `BINARY` resource 107 from the exe,
  each uint16 XORed with the one before it (the first with `0x45AD`), into a buffer from `operator new` (`+0` data,
  `+4` size). `scripts\us\global.res` in both GOG installs is that resource decoded, byte for byte. Format: uint32
  count, count entries {id, offset, flags (0/1/3/5), bytes} with strictly ascending ids, UTF-16 strings at
  `4 + offset`. DE and ENG have the same 23,123 ids, so the files are interchangeable.
- **Speech**: cMSS's constructor (`0x676200`, only called by the instance getter `0x6770E0` on first use) opens the
  256-byte `g_soundPakPath` (`0x9D760C`, initially `.\PAK\SOUND.PAK`) once and keeps the `FILE*` (`+0xB7F0`);
  `playSFX` (`0x693510`) reads every sample through it. `initApp` copies `.\PAK\SOUND.PAK` there with
  `cMSS_setPakPath` (`0x677160`) and creates cMSS, but the startup movies (`0x6A16E0`, called from `initApp` at
  `0x816D6C`, earlier) call the getter unguarded and may create it first. Header: `SND`, version 1, uint32 sound
  count (50,000 in both), 50,000 entries {flags, offset, size}, RIFF WAV samples. Sounds are named in the exe
  (`SOUND_FX_<name>` -> slot, table at `0x964870`, the same slots in both builds); the speech slots (hero lines such as
  `NOTMYITEM01_GLAD`, location remarks `SFX_LOC01_SCGLAD`) hold different recordings in the two paks, the effects
  are identical. The language argument passed to `cMSS_playSound` (`0x693FE0`) is unused. Music and ambience
  stream from `.\MP3\<name>.mp3` (`AIL_open_stream`).
- gameserver.exe loads neither. Besides the exes, the two GOG installs differ in these two files, `credits*.txt`
  (lists of text ids, different localization staff) and some data without text (`PAK\motions.pak` 85 bytes and 396
  longer in ENG, `bin\sets.bin` 259 bytes, 4 header bytes of each `startcode.bin`); movies, music, fonts and loading
  screens are the same.
