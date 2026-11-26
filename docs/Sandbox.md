# Sacred Gear UI sandbox

`gear_sandbox.exe` runs Sacred Gear's UI outside the game: the Lua host and kernel with the mods, the RmlUi host, and the replay of
the recorded commands, drawn through SacredBild's DirectDraw / Direct3D 7 emulation on Direct3D 9Ex (the path the game's UI takes
with SacredBild). The game's UI images and textures are read from the install's files, so documents that use `gear:image/N`,
`gear:texture/NAME.TGA` and the game's fonts look as they do in game. It opens a window, or renders off-screen and saves PNG
snapshots, and driver scripts step it frame by frame for automated UI tests. UI work no longer needs the game to start.

Build: the target `gear_sandbox` (CMake option `SACREDGEAR_SANDBOX`, on by default), `out\build\msvc-x86\RelWithDebInfo\gear_sandbox.exe`.

## What it is made of

- **Gear's own code.** Everything of SacredGear.dll but its `DllMain` and hooks is compiled once into the object library `gear_objs`;
  the DLL and the sandbox both link it. Game addresses stay 0 in the sandbox (no signature search runs), so every feature that needs
  the game is off, the way it is when a signature does not match. The exe sits at `/BASE:0x50000000`, away from where Sacred.exe would be.
- **SacredBild's Direct3D 7 emulation** (`src/bild/ddraw9`): the sandbox creates a DirectDraw object, a 32-bit render target of the
  canvas size and a Direct3D 7 device on it, and calls `Ui::hostDraw` for the three layers as the game's UI pass calls the overlay.
  The window shows the target by a `Blt` to the primary surface; `--hidden` never shows a window.
- **The game's pictures** (`tools/sandbox/game_data.*`): Sacred.exe holds the UI image table (0x54-byte records from `INVALID` to
  `INVALID_MAX`; sheet name and pixel rectangle, inclusive), `PAK\Texture.TMP` lists the textures and where they are (archive slot and
  offset in texture.pak or Texture00..15.pak), the archive entries are A4R4G4B4 (raw or zlib) or A8R8G8B8. The install is only read.
- **Seams in Gear** for a host other than the game, unused in game: `Ui::setHost` (images, UI state, screen bounds, placement),
  `Ui::hostMouse` / `hostWheel` (a click or wheel as the game's UI manager would hand it over), `Ui::hostDraw` / `hostDrawList`,
  `Ui::hostCapture` (a copy of every command added), `GearClock` (simulated time for `native.time` and RmlUi),
  `LuaHost::configureFolders`, `RmlHost::setFontDir`, and the test natives `rml_nocache` and `rml_stats`.

Layout units are canvas pixels in the sandbox (UI scale 1, the layout's corner at the canvas's corner); a canvas larger than
1024 x 768 is the screen extent documents see.

## Running

```
gear_sandbox [--game <dir>] [--mods <dir>] [--enable all|a,b,c] [--no-mods] [--size WxH] [--mode game|menus] [--hidden]
             [--frames N] [--snapshot out.png] [--background image] [--sim [dt]] [--log file] [--script file.lua [args...]]
```

- `--game`: the Sacred Gold install (default `B:\Spiele\GOG Games\sacred gold GOG`), read only.
- `--mods`: the mods folder; the kernel writes `gear_mods.json` and the layout there, so point it at a copy, not at a repository.
  `--enable` switches mods on before the start (`all`: every folder with a `mod.lua`).
- `--no-mods`: no kernel; a driver script drives the RmlUi natives itself.
- `--background`: an image (a game screenshot) under the UI.
- `--sim`: simulated clock, `dt` seconds per frame (default 1/60); scripts always run on it, so runs are reproducible.

## Driver scripts

`--script file.lua [args...]` runs a Lua script (LuaJIT) with these globals; the rest of the command line is its `arg` table, and a
number it returns is the exit code.

- `sbx.frame([n])`: n frames (kernel tick, then the three layers drawn); false once the window was closed.
- `sbx.advance(seconds)`, `sbx.time()`: the simulated clock.
- `sbx.mouse(x, y)`, `sbx.button(down [, button])`, `sbx.wheel(delta)`: input at the canvas position (button and wheel go to mod
  regions as in game; the mouse position is what `gear.ui.state()` reports). `sbx.key(vk, down)`: a key as mods see it (key bindings).
- `sbx.snapshot(file.png)`, `sbx.size()`, `sbx.image(id)` (sheet, width, height of a UI image).
- `sbx.commit()`, `sbx.draw()`: publish the commands recorded so far and draw them without a tick (scripts that record themselves).
- `sbx.clip(on, x, y, w, h)`: the clip of the commands recorded from now on (as `gear.ui.scroll_begin` sets it).
- `sbx.capture_begin()`, `sbx.capture_end()` -> handle, `sbx.capture_info(h)`, `sbx.capture_clear()`: copies of the commands added in
  between; `sbx.compare(a, b)` -> true or false and the first difference (retained lists expanded, meshes compared by content);
  `sbx.render_compare(a, b [, prefix])` -> differing pixels, largest channel difference (each capture drawn alone; with `prefix`, a
  difference is saved as `prefix_a.png` and `prefix_b.png`).
- `rml`: Gear's RmlUi natives (`rml.rml_load_string(mod_dir, markup, dp, model, calls)`, `rml.rml_update(id, x, y, hover)`,
  `rml.rml_draw(id, layer, slot)`, ...; see `src/gear/ui/rml.cpp`). Documents load from a mod folder of `--mods`.

## The RmlUi fuzzer

`tools/sandbox/fuzz/rml_fuzz.lua` tests retained-mode documents ([RmlRetained.md](RmlRetained.md)) against their own twin. Each case
generates a document (windows, nested elements, buttons, text, range and check inputs, images of the game and the mod, scroll boxes,
classes with `:hover` / `:active` / `:focus` rules, transitions, animations, transforms, a data model with `data-if`, `data-for`,
`data-class`, `data-attr`) and loads it twice: A runs as every document does, B with `rml_nocache` (updated, walked and rendered every
frame, as before retained mode). Random steps go to both alike (mouse moves, presses, wheel, keys, typed text, nav clicks and nudges,
every setter native, appends and removes, show, focus, scroll, move, resize, model values, pad scrolling, layers, clips, time), and
after each step both are updated, walked and drawn and must agree on events, the rows of `rml_rects` / `rml_nav` / `rml_scrolls` /
`rml_scrollbars`, the recorded commands, and every tenth step the pixels.

```
gear_sandbox --hidden --no-mods --mods tools\sandbox\fuzz\mods --script tools\sandbox\fuzz\rml_fuzz.lua seed=1 cases=60 steps=200 out=<dir>
```

Options: `seed`, `cases`, `steps`, `case=K` (only case K), `out=<dir>` (failing cases as `.rml`, `.steps.txt`, `.repro.lua` and PNGs),
`replay=<file.repro.lua>` (play a shrunk failure again; with `verbose=1` A's updates and renders per frame), `hide=1` (also hide
documents), `strict=1` (count short lags), `flicker=1` (count flicker), `stop=1`. The exit code is the number of failing cases.

A difference is followed by idle frames and classified: **lag** (A agrees again after n frames), **flicker** (B itself changes from
frame to frame: a document whose `:hover` rules push the element away from the mouse never settles; B flickers as before retained
mode, A stops on one state) or **stale** (B is stable, A stays different: a missed change). Lags of one or two frames and flicker are
not counted as failures: the hover of an element that moved under a still mouse settles one update later by design (principle 4 of
RmlRetained.md), while the twin moves the mouse before every update and gets it a frame earlier. A failing case is shrunk (ddmin) to the
few steps that still fail.

## Findings (2026-11-26)

| Finding | Kind | Status |
|---|---|---|
| A font atlas that one document's render rebuilds is released at once (`Ui::pictureRelease` erased the id): documents drawn earlier in the same tick, and the published frame the render thread still draws, lost their text or drew noise for one frame. Older than retained mode, more frequent with it (a replayed draw keeps the old atlas id until the release touches the document). | missed pixels, 1 frame | fixed: a released picture stays drawable for a few layer draws (`kReleasedPictureFrames`) |
| A trailing update that itself moved the hover (a `:hover` rule resized an element, another one came under the still mouse) was the last one: the document kept the in-between layout until the next input. | stale | fixed: an update that moved the hover adds one more, at most `kMaxSettle` (4) in a row |
| A hidden document (`rml_show(false)`) gets no update from RmlUi's `GetNextUpdateDelay`, so its animations stand still while it is hidden and continue from there when it is shown; before retained mode they ran on while hidden (if the mod kept calling `doc:draw`). Shown by a focused script (an animated box hidden for 3 s); the random cases with `hide=1` rarely meet it. | behaviour change | open: harmless for most UI, a choice to make |
| One-frame hover lags after layout changes under a still mouse. | lag 1 | by design (principle 4) |

After the fixes, 760 cases of 200 steps (seeds 1 to 12, 60 cases each, plus 40 with `hide=1`) agree, apart from a few one-frame hover lags
and documents that flicker on their own.

`tools/sandbox/examples/session_window.lua` is a UI test against real mods: the session tracker's window opened, its button hovered and
pressed, F2 closing it, a snapshot after each step.
