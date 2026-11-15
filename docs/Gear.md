# Sacred Gear

Sacred Gear is a mod kit for **Sacred Gold**: mods written in Lua (LuaJIT) that read the game's own data tables,
react to its events and, in later versions, change and extend them. It is a DLL of its own, `SacredGear.dll`, built
from this repository next to SacredBild but independent of it: it works with SacredBild, without it, and with any
other DLL loader.

Status: **milestone 2 (in progress)**. Mods load and run, get frame and session events, timers, tasks, custom events,
JSON, named pipes and a config file; they read the object and creature types and the world (the hero, creatures
nearby) and change it in single player: spawn creatures, teleport, set health and gold. An automation mode drives
the game without a player (load a savegame at start, simulated mouse and keys, UI state, screenshots) for self tests.
The world functions are new and still being checked in game. See [Roadmap](#roadmap).

Supported executable: the **English** GOG `Sacred.exe` (PE timestamp `0x452F85C7`). Sacred Gear finds the game's
code by byte signatures, like SacredBild, so other builds with the same code may work; if a signature isn't found,
Sacred Gear logs it and stays off. It only ever runs in `sacred.exe`, not in `gameserver.exe`.

## Install

Sacred Gear needs something to load it into the game. Any of these works:

| How | What to do |
|---|---|
| With SacredBild | Put `SacredGear.dll` into the game folder. SacredBild loads it at startup. |
| With an ASI loader (e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll` or `winmm.dll`) | Rename it to `SacredGear.asi` and put it where the loader looks for plugins (the game folder or its `scripts`/`plugins` folder). |
| With an injector | Inject `SacredGear.dll` into `sacred.exe`. Early is better: injected after a game started, the mods load with the next in-game frame. |

Mods go into the **`mods`** folder in the game folder, one folder per mod. Without a `mods` folder, or with an empty
one, Sacred Gear changes nothing. It logs to `SacredGear.log` in the game folder (the previous two runs are kept as
`.prev.log` and `.prev2.log`); an automation run logs into its own folder instead (see Automation).

If two copies end up in the game (say `SacredGear.asi` through a loader and `SacredGear.dll` through SacredBild), the
first one that loads is the active one and the other does nothing.

## Writing a mod

A mod is a folder under `mods` with two files:

```
mods\
  hello\
    mod.lua     the manifest
    main.lua    the code, run once when the mods load
```

`mod.lua` returns a table. It runs without any globals, so it can only hold values:

```lua
return {
    id = 'hello',              -- required: letters, digits, _ and -; unique among the installed mods
    name = 'Hello, Sacred Gear',
    version = '1.0',
    priority = 0,              -- lower loads first (default 0); ties load by id
    requires = {'other-mod'},  -- ids that must load first; the mod is skipped if one is missing
    enabled = true,            -- false: keep the folder, skip the mod
}
```

`main.lua` registers handlers for events:

```lua
gear.on('load', function()
    print('loaded ' .. gear.mod.id)
end)

gear.on('frame', function(dt)
    -- every in-game frame, dt in seconds
end)
```

`gear/examples/hello` in the repository is a complete example.

### When mods run

The mods load with the first in-game frame (with [automation](#automation) on: in the main menu already). All mod
code runs on one thread at a time: in game the engine thread, between the game's update and the drawing of the frame,
so handlers see the frame's state, never a half-updated one, and need no locks.

### mod.lua

| Field | |
|---|---|
| `id` | Required: letters, digits, `_` and `-`; unique among the installed mods |
| `name`, `version` | Shown in the log; `version` is part of the mod set |
| `priority` | Lower loads first (default 0); ties load by id |
| `requires` | Ids that must load first; the mod is skipped if one is missing |
| `enabled` | `false`: keep the folder, skip the mod |
| `config` | Default settings: `gear.config` starts as a copy, and `config.lua` in the mod's folder (a table, values only) overrides them. Players edit `config.lua`. |
| `pipes` | Names of the named pipes the mod may open (`gear.pipe.open`) |
| `automation` | `true`: the mod gets `gear.auto` when the game runs with `-gear-auto` |

### Lua API

| | |
|---|---|
| `gear.on(event, fn)` | Calls `fn` for `event`; returns a handle. |
| `gear.off(handle)` | Removes a handler. |
| `gear.emit(event, ...)` | Calls every handler of a custom event now. Custom names have a dot (`twitch.command`); `session.*`, `gear.*` and `pipe.*` are reserved. |
| `gear.after(s, fn)`, `gear.every(s, fn)` | Timers (real seconds while the game runs frames); return a handle for `gear.cancel(handle)`. |
| `gear.task(fn)` | Runs `fn` as a task that may wait: `gear.wait(s)`, `gear.wait_until(test, timeout)` (true when `test()` returned true, false after `timeout` seconds, default 30). |
| `gear.config` | This mod's settings (`mod.lua` `config`, overridden by `config.lua`). |
| `gear.json` | `encode(value)`, `decode(text)` (value, or nil and a message); `json.null` for null. |
| `gear.pipe.open(name)` | A client of `\\.\pipe\<name>` (listed in `pipes`), reconnecting by itself. `p:send(text)`, `p:connected()`, `p:close()`; set `p.on_message(text)`, `p.on_connect()`, `p.on_disconnect()`. Messages are framed with a 16-bit little-endian length (the risen-twitch / Gothic Twitch connector's format). |
| `gear.log(...)`, `print(...)` | A line in `SacredGear.log`, prefixed with the mod's id. |
| `gear.time()` | Seconds, high resolution, from an arbitrary start. |
| `gear.version`, `gear.mod` | Sacred Gear's version; this mod's `id`, `name`, `version`, `dir`. |
| `require(name)` | Runs `name.lua` of this mod's folder once (`a.b` is `a\b.lua`) and returns its result. |

Events: `'load'` (all mods loaded), `'frame'` (`dt`, every in-game frame), `'draw'` (`state`, after the frame handlers: see [UI drawing](#ui-drawing)), `'session.start'` (`hero`, a creature
object: a game with a hero runs), `'session.stop'` (that hero is gone: noticed with the next game's first frame when
the player left to the menu), `'idle'` (automation only: every 50 ms in the menus), and the custom events mods emit.

#### Data

| | |
|---|---|
| `gear.data.object_types` | The type records every object refers to (0x80 bytes; id = index, 1..0x7E5F): `:name(id)`, `:kind(id)`, `:find(name)` (id, any case), `:search(text, limit)` (`{id, name, kind}`), and the record reader below. |
| `gear.data.creature_types` | `Pak\Creature.pak`'s records (0x56 bytes). |

Both are read only and have `:count()`, `:stride()` and `:get(i)` (record `i` from 0, or nil); a record reads fields by
byte offset: `:u8(off)`, `:i8`, `:u16`, `:i16`, `:u32`, `:i32`, `:f32`, `:str(off, max)`; nil if out of range.

#### World

Positions are world units (`gear.world.TILE`, about 53.7, is one tile; the screen shows about 534 x 400 around the
hero) and a layer (0 outdoors; dungeons have their own). The functions that change the world work only in single
player (`gear.world.can_change()`); in a network game they return nil and a message.

| | |
|---|---|
| `gear.world.hero()` | The local hero (a creature object), or nil. |
| `gear.world.object(id)` | The object with `id`, or nil. |
| `gear.world.creatures_near(x, y, radius, {layer, limit})` | Creatures there, nearest first (default limit 64). |
| `gear.world.around(x, y, min, max)` | A random point between `min` and `max` from (x, y). |
| `gear.world.spawn(type, x, y, {layer, label})` | Creates an object of `type` (a type id or name) there, as the game's own scripts do; the creature, or nil and a message. `label` is kept for the creature this session (`c:label()`; shown in game in a later version). |

A creature object `c` (`c.id`) reads the game each time; after the object is gone the reads give nil:
`c:exists()`, `c:info()` (all fields), `c:type()`, `c:type_name()`, `c:position()` (x, y, layer), `c:hp()`,
`c:max_hp()`, `c:is_dead()`, `c:level()`, `c:experience()`, `c:gold()`, `c:distance(other | x, y)`, `c:label()`;
changes: `c:set_hp(hp)` (1..max: mods do not kill through it), `c:set_gold(n)`, `c:add_gold(n)` (heroes),
`c:teleport(x, y, layer)`.

#### Items and pickup

| | |
|---|---|
| `gear.world.item(id)` | An item object (weapon, armour, ring, amulet, potion, rune ...) as a table, or nil: `id`, `type`, `type_name`, `kind` (the type record's kind), `category` (`'weapon'`, `'armor'`, `'shield'`, `'jewelry'`, `'potion'`, `'rune'`, `'scroll'`, `'special'`, `'gold'`, `'other'`), `rarity` (0..15) and `quality` (`'normal'` 0-4, `'magic'` 5-11, `'rare'` 12-14, `'unique'` 15, or `'set'` for set items; the game's label colours white, blue, yellow, gold, green) with `quality_rank` (1..5 in `gear.world.QUALITIES` order), `set`, `classes` (the hero classes that can use it, empty: every class) and `class_mask`, `gold`, `quest`, `potion` (`'health'`, `'mana'`, `'stamina'`, `'colored'`, `'other'`) and `potion_color` (`'black'`, `'blue'`, `'green'`, `'red'`, `'yellow'`), `on_ground`, `x`, `y`, `layer`. |
| `gear.world.items_near(x, y, radius, {layer, limit})` | The items lying in the world there, nearest first (default limit 64). |
| `gear.world.usable_by(item, class)` | Whether `class` (`'darkelf'` ...) can use the item. |
| `hero:class()` | The hero's class: `'seraphim'`, `'gladiator'`, `'battlemage'`, `'darkelf'`, `'woodelf'`, `'vampiress'`, `'dwarf'`, `'daemon'` (and its id); nil for other creatures. `gear.world.CLASSES` lists them `{id, name, bit}`. |
| `gear.world.pickup_rules({block = {ids}, dim = {ids}, dim_alpha})` | This mod's pickup rules, replacing its previous ones (`nil`: none): the local hero does not pick up the `block` items (a click on them does nothing, the automatic collecting passes them by), and the ground labels of the `dim` items are drawn with their alpha scaled to `dim_alpha` (0..255, default 96). The rules of all mods are merged after each tick. |
| `gear.world.pickup_refused()` | Pickups refused so far. |

Pickup rules hold in single player and for the host's own hero; a network client's pickups go through the host
(its own message) and are not filtered. Item ids are only valid while the item exists: a mod that filters loot looks
at the items around the hero every fraction of a second and hands over the current list (`gear/examples/lootfilter`).

#### UI drawing

`gear.ui` draws on top of the game: translucent rectangles, text in the game's own fonts and the game's own UI
images (combat art icons, portraits, window parts), read-only for now. It is immediate mode: what a mod draws during
a tick (its `draw` and `frame` handlers, timers, tasks) is shown until the next tick replaces it, so a mod draws its
overlay every frame, usually in `gear.on('draw', function(state) ... end)` (`state` is `gear.ui.state()`). Without a
tick (a loading screen, the menus without [automation](#automation)) nothing is drawn after half a second.

Positions and sizes are the UI's **1024x768 layout**, the space the game lays its windows out in (and that
`gear.auto.ui()` reports); the game and SacredBild scale it to the screen. Colors are `0xAARRGGBB` numbers or
`{r, g, b, a}` tables (0..255, `a` defaults to 255); `gear.ui.rgba(r, g, b, a)` makes the number.

| | |
|---|---|
| `rect(x, y, w, h, color, opts)` | A filled rectangle (default color: black, 75 % opaque). |
| `frame(x, y, w, h, color, thickness, opts)` | An outline (default thickness 1) inside the box. |
| `text(text, x, y, opts)` | Text. By default in a vector UI font (Segoe UI) rasterized for the screen's pixels, so it stays sharp however far the UI is scaled: `size` (the em height in layout units, default 13), `face` (a Windows font name), `bold`, `italic`, `shadow` / `outline` (1 pixel, dark). `font = 'game1'` .. `'game7'` (or a number: 1..7 the UI's fonts, 0 a debug font) draws in the game's own bitmap font instead, scaled with the UI like the game's text. `color` (default white), `width` / `height` (the box: `align = 'left' \| 'center' \| 'right'` aligns in its width, `valign = 'top' \| 'middle' \| 'bottom'` in its height; `'middle'` puts the capitals' middle on the box's, on whole pixels; `'baseline'`: y is the first line's baseline), `background` (a color: a rectangle behind the text, sized to it) and `padding` around it, `markup = true` (game fonts: pass their backslash codes through; otherwise a `\` shows as `/`). Lines break at `\n`. UTF-8. |
| `image(id, x, y, w, h, opts)` | A UI image; `w`, `h` default to its own size. `opts.fit`: `'stretch'` (default: fills the box), `'contain'` (the whole image, aspect kept, centered in the box), `'cover'` (fills the box, aspect kept, the overhang cut off); `opts.color` tints it (white: as is; the alpha makes it translucent). |
| `images(filter)` | The UI images whose sheet name contains `filter` (any case): `{id, sheet, width, height, loaded}`. Ids are the game's image indices (stable for a build). |
| `image_info(id)` | One of those, or nil. |
| `fonts()` | The number of the game's UI fonts. |
| `text_size(text, font)` | Width and height of the text in the layout, or nil. `font`: a game font (`'game1'`, a number), or a table of `text`'s font options (`{size = 16, bold = true}`); nil: the default UI font. Game fonts in game only (measuring may add glyphs to the font). |
| `window(name)`, `windows()` | The game's in-game windows by control name (`UI_WND_INVENTORY`, `UI_WND_TASKBAR`, ...; see `docs/UI_SYSTEM.md`): `{name, slot, x, y, width, height, visible, drawn}` in the window's own layout coordinates; `drawn`: open and drawn this frame (not hidden behind a window the game draws alone). |
| `state()` | `{available, mode, exclusive, escape_menu, message_box, help, mouse_x, mouse_y, screen_left, screen_top, screen_right, screen_bottom}`: `mode` is `'game'`, `'menus'`, `'cinematic'` or `'none'`; `exclusive` the control name of the window the game draws alone (options, save, character, world map), if any; the cursor in layout coordinates; the screen's edges in layout coordinates (beyond 0..1024 x 0..768 when SacredBild's canvas leaves room around it). |
| `screen()` | Those four edges. |
| `WIDTH`, `HEIGHT` | 1024, 768. |

Rectangles, frames and images snap their edges to the screen's pixels (a 1-unit line is equally wide everywhere);
images are filtered linearly. Vector text is drawn from glyph atlases (1024x1024 textures, rasterized by GDI with
grayscale antialiasing at the font's size times the UI's scale, each glyph 1:1 on the pixel grid); without SacredBild
the layout is the screen and the same holds at scale 1.

`opts` of every draw function also take:

- `layer`: where in the game's draw order. `'world'`: in game, over the world and under every window. `'hud'`
  (default): over the game's windows, under tooltips, the escape menu and message boxes. `'top'`: over everything
  but the cursor and its tooltip.
- `window`: a control name. The command is drawn in that window's coordinates and frame, so it moves with the
  window when SacredBild lays out the HUD (`[UI] Anchor`), and it is clipped to that window's 1024x768 frame like
  the window itself; nothing is drawn while the window does not exist (in the menus). Without it the command uses
  the canvas and may reach beyond it to the screen's edges (see `screen()`).

A mod may issue 2000 draw commands per tick (widgets count their rectangles and texts).

#### Widgets and input

Widgets are immediate mode too: call them every frame with the current values; each draws itself, takes the mouse
over its rectangle and returns what the player did since the last frame. `id` names the widget within the mod.
Every widget takes `opts.tooltip` (shown after the cursor rests on it for 0.4 s), `opts.layer` and
`opts.enabled = false` (greyed out, no clicks). Positions are canvas coordinates (not `opts.window`).

| | |
|---|---|
| `button(id, x, y, w, h, text, opts)` | true when clicked. `opts.image`: a UI image beside the text. |
| `checkbox(id, x, y, text, checked, opts)` | `changed, checked`. |
| `radio(id, x, y, text, selected, opts)` | true when clicked. |
| `radio_group(id, x, y, options, index, opts)` | `changed, index`; `opts.horizontal`, `opts.spacing`, `opts.tooltips` (one per option). |
| `dropdown(id, x, y, w, options, index, opts)` | `changed, index`. The open list lies on the top layer; a click beside it closes it. |
| `slider(id, x, y, w, min, max, value, opts)` | `changed, value` while dragged; `opts.step` (default 1), `opts.format` (`'%g'`; false: no number). |
| `label(id, text, x, y, opts)` | Text in the widgets' look (the UI font; `size`, `font`, `height` / `valign` as for `text`), with a tooltip if `opts.tooltip`. |
| `panel(id, title, x, y, w, h, opts)` | A window: `{open, x, y, w, h, content_x, content_y}`. Its title bar drags it (the position is kept for the session), its X closes it (`open` false); `show_panel(id, open)` and `panel_open(id)`. `opts.closable`, `opts.draggable` (default true). |
| `region(id, x, y, w, h, opts)` | A rectangle that takes the mouse, for widgets of a mod's own: `{hover, pressed, released, clicked, held}`. |
| `mouse()` | The cursor in layout coordinates. |
| `key_pressed(name)` | True once per press of a key (`'f9'`, `'a'`, `'escape'` ...), read while the game's window has the focus. |
| `key_down(name)` | Whether the key is held. Both see the keys [automation](#automation) holds; isolated automation sees only those. |
| `COLORS` | The widgets' colours. |

Input: a left click (press and release) over a mod's region is taken from the game: the UI manager's event handler
(0x00757840) reports it handled, so neither the game's windows nor the world see it, and the release that follows
goes to the same region. Regions on the world and hud layers take nothing while the escape menu or a message box is
up. Hover is decided against the previous frame's regions (the topmost under the cursor), so an open dropdown hides
what lies below it. Keyboard input is only `key_pressed` for now (no text fields). A press or release also puts
`mouse()` at its own position for that tick.

While the cursor is over a mod's region (any widget, panel or the layout editor's overlays), the game's windows do
not see it: with SacredBild, their reads of the cursor give a point far off the screen, so they neither hover nor show
item tooltips under mod UI (the held item still follows the real cursor), and the world picks nothing there.

#### Layout: persistent panels, movable game windows

Every panel has an id, `mod_id:panel_id` (`panel().key`). Where the player drags it (and how large they make it, with
`opts.resizable`, `opts.min_width`, `opts.min_height`) is kept in `mods\gear_layout.json` and restored at the next
start. Places are stored in a virtual 4096 x 4096 screen (the whole screen, whatever its resolution and aspect), so a
layout keeps its places when the resolution changes. The game's own windows are kept there too, as
`game:UI_WND_INVENTORY` and so on, and placed every frame.

| | |
|---|---|
| `edit_mode()`, `set_edit_mode(on)` | The edit mode: a flag for every mod (panels get a highlighted frame); a layout editor turns it on and draws its overlays (`gear/examples/layout`, F7). |
| `panels()` | The panels of every mod drawn this tick (those not drawn yet: as of the last tick): `{key, mod, id, title, x, y, w, h, resizable, min_width, min_height, layer}`. |
| `set_panel_rect(key, x, y, w, h)` | Puts any mod's panel at (x, y) of the canvas and, if it is resizable, makes it w x h (`nil`: the size stays; never below its minimum); kept in the layout, taken the next time its mod draws it. False for an unknown panel. |
| `reset_panel(key)` | That panel back to the place and size its mod gives it. |
| `can_move_windows()` | Whether the game's windows can be moved (needs SacredBild's UI frames). |
| `set_window_position(name, x, y)` | Puts game window `name` with its top-left corner at (x, y) of the canvas and keeps it in the layout. Any in-game window but those the game draws alone (world map, savegame, options, character export). |
| `reset_window(name)`, `reset_layout(prefix)` | Back to its own place; forget all places (or those whose key starts with `prefix`, e.g. `'game:'`). |
| `layout()` | The stored places `{key = {x, y, w, h}}` (virtual units). |
| `to_virtual(x, y)`, `from_virtual(vx, vy)` | Between the canvas and the virtual screen. |

`windows()` entries have `frame_x`, `frame_y` (where the window's frame puts its layout: its place on the canvas is
`x + frame_x, y + frame_y`), `movable` and, with SacredBild, `bounds = {x, y, w, h}`: where the window drew in the
last frame, on the canvas, its tabs and ornaments beyond its rect included (the inventory's page tabs, the portrait of
`UI_WND_MERC` without the minimap's empty strip). Names come from the window's slot (the name field of some windows
reads empty or holds junk).

A game window is moved by giving it a UI frame of its own in SacredBild (exports `SacredBild_UiSetWindowFrame`,
`SacredBild_UiGetWindowFrame`): its drawing, its 3D items, the cursor it reads, the clicks it gets and its hit tests
all move with it, as with SacredBild's HUD anchoring. Without SacredBild the game's windows stay where they are; mod
panels move either way.

Files: `gear.read(name)` reads a file of the mod's folder, `gear.save(name, text)` writes one (directly in the mod's
folder, `*.json`, `*.txt` or `*.lua` other than `mod.lua` and `main.lua`, at most 1 MB): for settings a mod's own UI
changes.

```lua
gear.on('draw', function(state)
    if state.mode ~= 'game' or state.exclusive then return end
    local ui = gear.ui
    ui.rect(16, 16, 200, 40, ui.rgba(0, 0, 0, 150))
    ui.text('Hello', 24, 24, {color = 0xFFFFD080})
    ui.image(ui.images('GUI_spell01')[1].id, 170, 20, 32, 32, {fit = 'contain'})
end)
```

`gear/examples/uidemo` shows all of it.

The standard libraries a mod gets: `string`, `table`, `math`, `bit`, `coroutine`, and from `os` only `clock`, `time`,
`date` and `difftime`. There is no `io`, `os.execute`, `debug`, `load`/`loadstring`/`dofile`, `jit` or `ffi`: a mod
reads only its own files (through `require`) and cannot reach the rest of the computer. Each mod has its own globals.

### Errors

An error in a mod's code is logged with its traceback and does not reach the game. A mod whose handlers raised ten
errors is switched off until the game restarts. A handler that runs too long (an endless loop: 0.25 s for a frame's
handler, 5 s for loading) is stopped with an error; LuaJIT checks this only in interpreted code, so a loop the JIT
compiled may not be caught. The game's own functions that Gear calls for a mod run under a fault guard: a call that
faults returns nil and a message instead of crashing the game.

### Examples

`gear/examples` in the repository:

| Mod | |
|---|---|
| `hello` | The smallest mod: events and the creature types. |
| `twitch_integration` | The Twitch connector's pipe, mapped by `custom.lua` to actions, offered one at a time inside a game as `twitch.command`. |
| `twi_sacred` | The actions: monsters spawned just out of sight and labelled "<viewer>'s <monster>", gold, hit points (also for a few seconds), teleport. Set `config.monsters` to type names of this game (the selftest writes them all). |
| `uidemo` | `gear.ui`: a translucent hero panel, a widget gallery in a draggable, closable panel (F9) with every widget, images in the three fit modes and the fonts, tooltips; a note in the inventory's frame, the world and top layers; with `-gear-auto` a self test that clicks every widget and takes screenshots. |
| `lootfilter` | A loot filter: the hero picks up only items for its class (or chosen classes), above a minimum quality, with per-category rules and potion choices from `config.lua`; filtered items' labels are dimmed; an overlay shows the rules; a settings panel (F8 or the overlay's button: class checkboxes, quality dropdowns, potion checkboxes, label dimming) saves to `settings.json`. Self test with `-gear-auto`. |
| `layout` | A layout editor in the manner of an MMO's edit mode (F7 or its button): every open game window (all it draws, its tabs too) and mod panel gets a translucent blue overlay with its name, and the game's tooltips stay hidden under it; click one to select it (gold) and drag it anywhere on its overlay; it snaps to the screen's edges and centre and to the other frames (Shift: freely); arrow keys nudge the selected one by a virtual unit (Shift: 10); its popup shows the place, resets it and sizes resizable panels (or drag the corner); a click on empty space deselects. Kept in `mods\gear_layout.json`. Self test (run it twice: select, drag, nudge, resize, then check the restore and reset). |
| `selftest` | An automation run: loads a savegame, checks the hero, gold, nearby creatures, types, optionally a spawn, writes `GearAutomation\selftest.json` and `types.txt`, screenshots, quits with 0 or 1. |

## Automation

For self tests and scripted runs without a player. Off unless the game starts with `-gear-auto` (or
`SACREDGEAR_AUTO=1` in its environment). Then:

| Option | |
|---|---|
| `-gear-load=N` (`SACREDGEAR_LOAD`) | Loads savegame N (`Save\GAMEnn.PAK`, 0 the quicksave) as soon as the main menu is up, the way the menu's savegame window does it. |
| `-gear-mods=a,b` (`SACREDGEAR_MODS`) | Loads only these mods. |
| `-gear-instance=NAME` (`SACREDGEAR_INSTANCE`) | Names the run, default `pid<PID>`. See "Parallel runs". |
| `-gear-window=WxH[+X+Y]` (`SACREDGEAR_WINDOW`) | SacredBild only: the size of the game window, default 1600x900, at least 1024x768 (smaller values are raised to it). See below. |

The mods load in the main menu and get the `idle` event there. Mods with `automation = true` get `gear.auto`:

| | |
|---|---|
| `state()` | `{ui, menus, in_game, cinematic, help, dialog, window, width, height, mouse_x, mouse_y}`: the UI manager's state, the client size, the game's cursor in UI layout coordinates. |
| `ui()` | The visible UI: menus, in-game windows, the message box and their visible children, each `{group, index, parent, vtable, flags, x, y, width, height}` in UI layout coordinates (`parent` indexes the list, 0 top level). |
| `move(x, y)`, `down(button)`, `up(button)`, `key_down(key)`, `key_up(key)`, `release()` | Raw input; positions in client pixels; buttons `'lbutton'`, `'rbutton'`, `'mbutton'`; keys by name (`'escape'`, `'enter'`, `'f5'`, `'a'`, ...) or virtual key code. |
| In a task: `click(x, y, button)`, `press(key)`, `type(text)` | As a player would, with short pauses. |
| In a task: `ui_click(x, y)` or `ui_click(node)` | Clicks a point in UI layout coordinates (or a node's center); the mapping to pixels is measured once by moving the cursor, so it works with SacredBild's scaled UI too. |
| `load_save(n)` | Loads savegame n from the menus. |
| In a task: `wait_for_menu(timeout)`, `wait_for_game(timeout)` | Until the main menu is up / a hero exists. |
| `instance` | The run's name (a string). |
| `screenshot(name)`, `write(name, text)` | Files in `<game>\GearAutomation\<instance>` (name: letters, digits, `_ - .`). |
| `quit(code)` | Ends the game at once with that exit code. |

Automation is isolated by default, so a test can run while you keep using the PC: the game never comes to the front
or takes the focus (its window calls to `SetForegroundWindow`, `SetFocus`, `SetCapture`, `ShowWindow`,
`SetWindowPos` and so on are turned into non-activating ones), it ignores the real mouse and keyboard (their window
messages are dropped, `GetCursorPos`, `GetAsyncKeyState`, `GetKeyState` and `GetKeyboardState` report only the
injected input, `SetCursorPos` and `ClipCursor` never touch the real cursor, low level input hooks are not installed),
and it believes it is the foreground window and active (it never sees itself deactivated, so it does not pause). Injected input reaches it through its own message queue. Run the game in
a window (for example with SacredBild's windowed mode): an exclusive fullscreen display takes the screen whatever
Gear does. `-gear-shared-input` (`SACREDGEAR_SHARED_INPUT=1`) turns the isolation off: the game then takes real
input too and the cursor is virtual only until the real mouse moves. These hooks are import hooks on `sacred.exe`
only. The foreground it believes in is a virtual one: the window it asked to activate (`SetForegroundWindow`, `SetFocus`, `SetActiveWindow`, `BringWindowToTop`), else its first top-level window; `GetForegroundWindow`, `GetFocus` and `GetActiveWindow` answer with it, so the startup wait for the foreground ends. SacredBild asks `SacredGear_IsolatedForeground()` (an optional export of `SacredGear.dll`) and counts the game as active then, so it does not swallow the injected keys.

`screenshot(name)` reads the game's own back buffer in-process (read-only lock of the DirectDraw surface; works with the vanilla ddraw and SacredBild's d3d9 backend, 16 and 32 bit) on the thread that renders: the engine's frame thread in game, the window's thread in the menus. It needs neither focus nor a visible window.

In the menus the back buffer is empty between some frames (the Direct3D 9 backend does not keep it), so the screenshot asks again until it holds a picture (up to 40 tries), and falls back to the primary surface. Characters (`WM_CHAR`) are not dropped as real input while isolated: the game takes its hotkeys from the ones `TranslateMessage` makes out of the injected key messages. The virtual cursor starts in the middle of the window.

A watchdog ends a run that takes too long: `-gear-timeout=SECONDS` (`SACREDGEAR_TIMEOUT`, default 300, 0 = off) quits with exit code 3 and logs `Automation: watchdog timeout`; the same if the game window does not answer, or an in-game run has no frame, for 60 s. Run tests with a timeout below the time you allow them.

**Window.** An automation run with SacredBild never goes fullscreen or desktop-sized and ignores `[Display] Width`,
`Height` and `Borderless` in `SacredBild.ini` (which stays untouched): it opens a normal framed window with a
1600x900 client area, in the bottom-right corner of the primary monitor's work area, without activating it.
`-gear-window=WxH` changes the size, `-gear-window=WxH+X+Y` also puts the client area's top-left at screen pixel X,Y.
SacredBild reads this from the command line itself; Gear does not know about it.

Example, from a shell: `Sacred.exe -gear-auto -gear-load=1 -gear-mods=selftest -gear-instance=a`, then read
`GearAutomation\a\selftest.json` and the exit code.

### Parallel runs

Every automation run is an *instance* with a name: `-gear-instance=NAME` (`SACREDGEAR_INSTANCE`; letters, digits,
`_ - .`, at most 48 characters, anything else becomes `_`; default `pid<PID>`, so even runs without a name never meet).
A normal launch (without `-gear-auto`) has no instance and keeps every vanilla behaviour, the single-instance check
included. What a name changes:

- **The single-instance check.** The game's start-up (`0x817B30`) creates the mutex `SACRED_INSTANCE`, and a second
  `sacred.exe` that finds it exists shows "ERROR: Sacred is already running" and exits. In an automation run Sacred Gear
  hooks the import of `CreateMutexA` (and `OpenMutexA`) in `sacred.exe` and names it `SACRED_INSTANCE-gear-NAME`: runs
  with different names never collide, nor with a player's game. The same name twice still does ("already running").
  There is no window-class or title check for the main window; the game finds only its gameserver's window class
  (`SACREDGAMESERVERCLASS`, LAN games), which is why automation runs should not host LAN games in parallel.
- **The window title** gets the suffix ` [gear:NAME]`.
- **The folder `<game>\GearAutomation\NAME\`** holds everything the run writes: `screenshot()` and `write()` files (`selftest.json`,
  `types.txt`, ...), `SacredGear.log` (with `.prev` rotation as usual), `SacredBild.log` and
  `SacredBild-crash-*.dmp` when SacredBild is loaded (both modules read the same name from the command line, so
  neither needs the other), and `run.json`.
- **`run.json`** is written when the run starts and rewritten when it ends:
  `{"instance": "a", "pid": 1234, "command_line": "...", "started": "2026-11-15T17:00:00.000Z"}` plus, at the end,
  `"exit_code": 0, "ended": "...", "reason": "quit"`. The reason is `quit` (a mod called `gear.auto.quit`),
  `watchdog` (exit code 3) or `exit` (the game ended itself, through `ExitProcess`). A `run.json` without `ended` is a
  run that is still going, or that was killed or crashed. The `pid` is the real process of that run (the one whose
  command line carries the name), so a runner can tell which process belongs to which step, and match
  `Win32_Process.ProcessId` or the window title.

Still shared between parallel runs, because the game or the game folder owns it: `Save` (a run that saves
overwrites the slot other runs load: load only, or use a copy of the folder), the game's own `Settings.cfg`,
`DEBUG.LOG`/`ExpLog.LOG` and `bin\sg*.bin` writes, `SacredBild.ini`, `SacredBild-profile.txt`, the mods'
own files (`mods\gear_layout.json`, a mod's `settings.json`), and the game folder itself (copy `ddraw.dll` and
`SacredGear.dll` once, before starting the runs; they are only read). Many runs also share one CPU and GPU: frame
times in a self test mean little, results do.

## Compatibility

- No mods: Sacred Gear installs its hooks (the engine's per-frame zoom update; for mod drawing the UI manager's popup
  pass and two calls inside its render; for pickup rules the item pickup, the collect filter and the item label
  colour; see Engine notes), calls the game's code unchanged through them and does
  nothing else. Savegames, multiplayer and the game's behavior are the vanilla ones. Automation adds import hooks on
  `sacred.exe`'s input and window functions (see Automation) and a message hook on the game window, only when it
  is on.
- Sacred Gear never stores anything in the game's savegames. State that mods will keep per savegame (a later
  milestone) goes into files of their own next to the savegame, so removing a mod never breaks a save.
- SacredBild and Sacred Gear never patch the same bytes: `tools/check_gear_overlap.py` checks every signature window
  of the one against every one of the other, so they work in either load order.
- Multiplayer is not supported by mods yet. Sacred Gear logs a hash of the loaded mods (ids and versions) as "mod
  set"; a later milestone sends it in the connection handshake so that a game only accepts peers with the same mods.

## Roadmap

| Milestone | Contents |
|---|---|
| M1 | `SacredGear.dll`, any loader, LuaJIT host, `mods` folder, `load` and `frame` events, creature types read only |
| M2a (now) | Sessions, timers, tasks, custom events, JSON, pipes, config; object types; world: hero, creatures, spawn, teleport, health, gold; automation. |
| M2b: data | Item, weapon and balance tables with named fields (a schema per table, checked against the record size); changing records; a file overlay so mods ship replacement data files; mod text; mods loading before the game's data |
| M3: events | Creature spawn and death, damage (with hooks that can change or veto it), item pickup and drop, save and load; per-savegame mod data |
| M4: content | New records (items, creature types); a bridge to the game's quest and trigger scripts |
| M5: tools | More examples, a reference of every table field, an in-game mod list; the multiplayer handshake |

## Development

Sources are in `gear/`; Sacred Gear links the code it shares with SacredBild (log, patches and hooks, signature
search) from the `sacred_core` library (`src/fmt.*`, `log.*`, `patch.*`, `sig.*`).

| Path | What it is |
|---|---|
| `gear/main.cpp` | DLL entry: in `sacred.exe` only, one instance per process, signatures, hooks |
| `gear/hooks.*` | The engine hooks: the per-frame tick |
| `gear/world.*` | Objects through the game's object manager: hero, creatures, spawn, teleport, health, gold |
| `gear/pipe.*` | Named pipe clients, each on a thread of its own |
| `gear/automation.*` | Automation: options, input, UI state, savegame loading, screenshots |
| `gear/pickup.*` | Pickup rules: the hooks on the game's pickup, the collect filter and the item label colour |
| `gear/ui.*` | Mod drawing: the draw commands, the hooks in the UI pass, Direct3D 7 quads with the game's states saved and restored, the game's fonts and UI images, window and UI state queries |
| `gear/tests/kernel_test.lua` | Tests of the kernel with the native side mocked: `luajit gear/tests/kernel_test.lua` |
| `gear/lua_host.*` | The LuaJIT state on the engine thread, native functions for the kernel, time budget |
| `gear/lua/kernel.lua` | The trusted Lua side (embedded as a resource): manifests, load order, sandboxes, events, table views |
| `gear/tables.*` | The engine's data tables, read in place with bounds and fault checks |
| `gear/game/gear_addr.h`, `gear_sigs.inc` | The sacred.exe addresses Sacred Gear uses; their signatures (generated) |
| `gear/examples/` | Example mods |

Tools (Python 3 with `pefile` and `capstone`, `SACRED_ENG` as for SacredBild's tools):

| Tool | Does |
|---|---|
| `tools/gen_gear_sigs.py` | Writes `gear/game/gear_sigs.inc` from the entries listed in it |
| `tools/check_gear_overlap.py` | Fails if a Sacred Gear signature window overlaps one SacredBild patches (or one of SacredBild's overlaps one Gear patches) |

Build: part of the normal build (`cmake --build --preset release` -> `out/build/msvc-x86/RelWithDebInfo/SacredGear.dll`);
`-DSACREDGEAR=OFF` leaves it out. vcpkg provides LuaJIT, statically linked like everything else.

## Engine notes

What Sacred Gear relies on, English GOG addresses:

| | |
|---|---|
| Object manager | `*0x00AD5C40`; +4 / +8 the object vector by id (ids 1..16 the heroes). Objects: +0x0C id, +0x10 type, +0x14 flags (0x10 freed or parked), +0x18 position `{u16 sector, i32 x, i32 y, u8 layer}`. Creatures: +0x3B4 experience, +0x3EE gold, +0x3FE level, +0x4D4 / +0x4D8 maximum / current health. |
| `cObjectManager::create` 0x005FBA40 | thiscall (type, position*, 0, broadcast, x) -> id; the hero is created with (type, pos, 0, 1, 0), the script command CreateNPC with (type, pos, 0, 0, x). |
| `cObjectManager::pop` 0x005FCCB0 | thiscall (id, position*): places an object (teleport); ParkCreature (0x006073D0) moves one far away with it. |
| Object types | `*0x00AAB5E4` + id * 0x80 (0 < id < 0x7E60): +0x2E kind, +0x37 name. |
| Loading a savegame | The menu (0x0071CC90 action 6) sets the players object's (`*0x0182EBE8`) +0x14 to 1 and posts message 0x8002 (wParam 1, lParam the savegame number) to the game window (`*0x00CDCA1C` +0xB0) (0x007561E0). |
| UI manager | `*0x017EEBBC`: +0x08 flags (1 menus, 4 in game, 0x10 cinematic, 0x200 help), +0x80 the in-game windows, +0x130 the five menu screens, +0x144 the message box. |
| UI pass (mod drawing) | `cUI_Manager_render` (0x00758ED0, arg: the IDirect3DDevice7 the UI draws with; SacredBild hooks it) draws pretransformed FVF 0x1C4 quads in the 1024x768 layout. Gear draws from: the popup pass 0x00758DC0 (thiscall (device, flag), "hud", before the original), the call at 0x0075934F (the party arrows 0x006D01F0, cdecl (device), "world", before the original; reached in game only) and the call at 0x00759585 (`call 0x649BE0`, after the escape menu and the message box in every mode, EBX = device; "top"). Every render state, texture stage state and the stage 0 texture it changes are read before and set back after, so the game's state cache (0x00643430) and SacredBild's state shadow stay right; text runs through the game's font with the game's states. |
| Fonts | `*0x00CDCA70`: a vector of `cFont*` by id. Vtable +0x18 `draw(device, cRect*, const wchar_t*, ARGB, flags)` (cRect: i32 x, y, i16 w, h; flags 1 centered and 4 right-aligned in the rect's width, 0x20 curved, 0x200 measure only), +0x28 `lineHeight(ch)`, +0x2C `textWidth(text)` (cFontTTF2 only). Backslash codes in the text: `\c` color, `\g` an inline UI image, `\r` reset, `\w`. |
| UI images | 0x54-byte records from 0x009ECEB0 (ids = indices, 1..1885; 1886 is "INVALID_MAX"): +0x08 the sheet's TGA name, +0x28 texture handle (`cTextureManager_get` 0x0065ED90 on `*0x013E7838`, +0x14 the surface), +0x2C..+0x38 u0, v0, u1, v1 (pixels until 0x761580 normalizes them), +0x4C / +0x50 width / height. |
| SacredBild (optional) | `ddraw.dll` exports `SacredBild_UiPushFrame(window, confine)`, `SacredBild_UiPopFrame()`, `SacredBild_UiScreenBounds(float[4])`, `SacredBild_UiSetWindowFrame(slot, x, y, set)` and `SacredBild_UiGetWindowFrame(slot, float*, float*)`, `SacredBild_UiPlacement(float[3])` (screen pixels per layout unit and the frame's screen origin: vector text is rasterized for it, rectangles and images snap to its pixels), `SacredBild_UiGetWindowBounds(slot, float[4])` (where a window
drew in the last frame, in its own layout) and `SacredBild_UiSetCursorFilter(fn(x, y))` (Gear tells whether mod UI lies
at a canvas point; SacredBild hides the cursor from the UI's reads there and blocks the world pick) (a window's own frame, any offset; SacredBild's window handlers now run whenever its canvas is on, [UI] Anchor or not); Gear draws a command with `window` in that window's UI frame and the others in the unconfined canvas. Without them (no SacredBild) the layout is drawn as the game draws it. |
| Items | An item is a `cItemBase` (RTTI type descriptor 0x0094EBC0). Its +0x118 holds a copy of its item record's data; the dword +0x11C: class mask (bits 0-5 Seraphim, Gladiator, Battle Mage, Dark Elf, Wood Elf, Vampiress; 0x400000 Dwarf; 0x800000 Daemon; 0 or 0xC0003F every class) and rarity (bits 16-19). Item records: `*(typeManager + 0x563004) + index * 0x102` (index: the type record's +0x18; count at +0x563000), +0x23 nonzero: set item. Object flags +0x14: 0x02 not in the world (inventory, being picked up), 0x10 freed. Type record +0x00 bit 24: quest item. Gold: types 0x140C-0x140F. Potions: 0x13C0-0x13C8 health / mana / stamina bottles (three sizes each); 0x1411, 0x1416, 0x1430: large, medium, small coloured potions (black, blue, green, red, yellow); which colour does what is not checked. The hero's class is its type (1..9, 7 unused). |
| Pickup | `cCreature::pickupItem` 0x00558080 (thiscall (creature, item id) -> 1 / 0, ret 4) is the only code that moves a world item to a creature: the click pickup (0x0053CEB0), the collect behaviour (0x004ECF69, 0x004ED095) and the host's message 0x20C call it and ignore the result. Gear returns 0 there for a blocked item and the local hero. The collect behaviour's filter 0x004EC330 (thiscall (creature, cObject*) -> bool; settings.cfg PICKUPAUTO) returns false for them. A network client sends message 0x141 instead (0x0053A170). Item label colour: 0x005C13C0 (thiscall (item, record or 0) -> ARGB): set green 0xFF15FF25; rarity 0-4 0xFFEFEFEF, 5-11 0xFFA0A0FF, 12-14 0xFFF0E040, 15 0xFFC19A33; class-restricted items take their class's colour unless UNIQUE_COLOR is on. |