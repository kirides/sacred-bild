# Display, UI and input

What SacredBild changes about how Sacred looks and behaves on a modern system. The settings named here are in
[`SacredBild.ini`](Configuration.md).

## Any resolution

Default: the desktop resolution, instead of the hard-coded 1024x768.

- Back buffer and window at the target size: borderless when it fills the screen, otherwise with a frame (caption
  and system menu: it can be moved and minimized) around a client area of that size (`[Display] Borderless`). Its
  close button is disabled so a stray click doesn't end the game (Alt+F4 still quits).
- A window with a frame can be resized (`[Display] Resizable`, [see below](#resizable-window)).
- Automation runs (`-gear-auto`) always use a framed 1600x900 window at the work area's bottom-right, whatever the
  settings say; `-gear-window=WxH[+X+Y]` overrides ([Gear](Gear.md#automation)).
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
- The cinematic letterbox bars run across the whole screen.
- Always a 32-bit display mode (`GFX32 : 0` in `Settings.cfg` is ignored).

## Resizable window

A window with a frame (not the borderless one) has a sizing border and a maximize button. The game keeps its
resolution and its back buffer: changing those while the game runs would mean releasing and recreating every
Direct3D 9 resource of the game, which its two presenting threads don't survive. So Windows stretches the presented
frame to the client area (the flip model's compositor, or Direct3D 9's own stretch with `VSync=0`), and SacredBild
only keeps that from distorting the picture and the mouse from missing it:

- The window keeps the aspect ratio of the game's resolution: dragging an edge or corner, snapping and
  programs' `SetWindowPos` calls all end at a client area of that ratio (within a pixel). The smallest window is a
  quarter of the resolution.
- "Maximize" (the button, a double click on the title bar, Alt+Space, dragging to the top edge) makes the largest
  such window that fits the monitor's work area, centered; maximizing again restores the size before. It is not
  Windows' maximized state, which would stretch the frame over the whole work area.
- The game reads and sets the cursor in screen pixels (`GetCursorPos`, `SetCursorPos`, also to keep it on its
  world spot while the view scrolls): both are converted between the window and the game's frame, so the UI, the
  world pick and the controller's cursor see the pixels of the frame. SacredBild's own code and the window itself
  keep the real coordinates (`ClipCursor`, the settings screens, whose ImGui is drawn in the back buffer and
  stretched with the frame).
- The back buffer, the UI canvas and anchors, SMAA and the texture filter work in the game's resolution and are
  not touched: to see more of the world, set `Width` / `Height`.

No game function is replaced for this: `GetCursorPos` is the game's import (wrapped the way Sacred Gear's
automation wraps it), `SetCursorPos` the one `Focus` already wraps, and the rest is the window procedure of the
main window that SacredBild already subclasses.

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
