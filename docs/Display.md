# Display, UI and input

What SacredBild changes about how Sacred looks and behaves on a modern system. The settings named here are in
[`SacredBild.ini`](Configuration.md).

## Any resolution

Default: the desktop resolution, instead of the hard-coded 1024x768.

- Back buffer and window at the target size: borderless when it fills the screen, otherwise with a frame (caption
  and system menu: it can be moved and minimized) around a client area of that size (`[Display] Borderless`). Its
  close button is disabled so a stray click doesn't end the game (Alt+F4 still quits).
- World view: orthographic projection (including its depth range), visible area, culling and overhead-label layout
  scaled so the world keeps its original pixel density and simply shows more (including the 0.5x-2.0x zoom).
- World view centered on the screen (the game assumed a 512/384 screen center in 17 places).
- UI: the game's 1024x768 UI (menus, HUD, cursor, intro videos) is drawn into a centered canvas, scaled to fit the
  screen height (`[UI] Scale`, `ScaleMode`, `LinearFilter`); the mouse is mapped into that canvas for the UI while
  world picking keeps physical screen coordinates.
- In game, the HUD windows are placed on the screen (`[UI] Anchor`, positions in `[UI.Layout]`); by default at the
  edges they had in the 1024x768 layout: taskbar and chat at the bottom, inventory bottom-left, minimap, character
  stats and equipment top-right, party portraits and the shop / chest / cube / trade windows top-left. Each keeps
  its own layout and runs in a shifted copy of the 1024x768 space (drawing, cursor, clicks, tooltips), so the
  game's code for it is unchanged; menus and full-screen windows (map, options, save) stay centered. Tooltips and
  the item on the cursor use the whole screen, the help screen's texts (H key) stay next to the windows they
  explain, and the escape menu and message boxes dim the whole screen.
- Loading screen (GDI) drawn into a 1024x768 surface and scaled like the menus, splash centered; savegame
  thumbnails taken from the screen center.
- Always a 32-bit display mode (`GFX32 : 0` in `Settings.cfg` is ignored).

## Screenshots

Print Screen saves the whole screen as `Capture\shotNNNN.png` (or `.jpg`, `[Screenshot] Format`), on a worker
thread; the game's own wrote the top-left 1024x768 as TGA + JPEG, sheared at other widths.

## Input only in the foreground

The game polls keys and mouse buttons whether or not it has the focus, moves the cursor, and its low-level keyboard
hook swallowed the Windows keys, Alt+Tab and Alt+Esc system-wide. All of that now only happens while the game is in
the foreground (its cursor still follows the mouse over the window, so you see where a click will land), so typing
in another window doesn't move your character, and Alt+Tab / Alt+Esc work in game too (the Windows keys and
Ctrl+Esc stay blocked while it has the focus).

The mouse is confined to the game window while it is in the foreground (`[Display] ClipCursor`), e.g. a window on
one side of a 32:9 screen or a borderless game next to a second monitor; it is let go while the window is moved
(Alt+Space, Move) or a menu of it is open, and while Alt is held; it is only taken once the cursor is over the
window, so a click on the title bar still drags.

## Frames in the background and the frame limiter

The game stopped drawing when it lost the focus, which often left the main menu black. It now keeps drawing at
`[Display] FpsLimitInactive` frames per second.

The game's limiter (60 fps in game and in the menus) spun on `Sleep(0)` for the rest of every frame. SacredBild
sleeps on a high-resolution timer instead.
