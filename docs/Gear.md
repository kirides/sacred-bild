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
`.prev.log` and `.prev2.log`).

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
| `text(text, x, y, opts)` | Text in one of the game's fonts. `opts`: `color` (default white), `font` (an id: 1..7 are the UI's fonts, 1 the default; 0 is a debug font), `width` / `height` (the box; `align = 'left' \| 'center' \| 'right'` aligns inside its width), `background` (a color: a rectangle behind the text, sized to it) and `padding` around it, `markup = true` (pass the fonts' backslash codes through; otherwise a `\` shows as `/`). UTF-8. |
| `image(id, x, y, w, h, opts)` | A UI image; `w`, `h` default to its own size. `opts.fit`: `'stretch'` (default: fills the box), `'contain'` (the whole image, aspect kept, centered in the box), `'cover'` (fills the box, aspect kept, the overhang cut off); `opts.color` tints it (white: as is; the alpha makes it translucent). |
| `images(filter)` | The UI images whose sheet name contains `filter` (any case): `{id, sheet, width, height, loaded}`. Ids are the game's image indices (stable for a build). |
| `image_info(id)` | One of those, or nil. |
| `fonts()` | The number of UI fonts. |
| `text_size(text, font)` | Width and height of the text in the layout, or nil if the font does not measure. In game only (it may add glyphs to the font). |
| `window(name)`, `windows()` | The game's in-game windows by control name (`UI_WND_INVENTORY`, `UI_WND_TASKBAR`, ...; see `docs/UI_SYSTEM.md`): `{name, slot, x, y, width, height, visible, drawn}` in the window's own layout coordinates; `drawn`: open and drawn this frame (not hidden behind a window the game draws alone). |
| `state()` | `{available, mode, exclusive, escape_menu, message_box, help, mouse_x, mouse_y, screen_left, screen_top, screen_right, screen_bottom}`: `mode` is `'game'`, `'menus'`, `'cinematic'` or `'none'`; `exclusive` the control name of the window the game draws alone (options, save, character, world map), if any; the cursor in layout coordinates; the screen's edges in layout coordinates (beyond 0..1024 x 0..768 when SacredBild's canvas leaves room around it). |
| `screen()` | Those four edges. |
| `WIDTH`, `HEIGHT` | 1024, 768. |

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
| `label(id, text, x, y, opts)` | Text in the widgets' look (font 7), with a tooltip if `opts.tooltip`. |
| `panel(id, title, x, y, w, h, opts)` | A window: `{open, x, y, w, h, content_x, content_y}`. Its title bar drags it (the position is kept for the session), its X closes it (`open` false); `show_panel(id, open)` and `panel_open(id)`. `opts.closable`, `opts.draggable` (default true). |
| `region(id, x, y, w, h, opts)` | A rectangle that takes the mouse, for widgets of a mod's own: `{hover, pressed, released, clicked, held}`. |
| `mouse()` | The cursor in layout coordinates. |
| `key_pressed(name)` | True once per press of a key (`'f9'`, `'a'`, `'escape'` ...), read while the game's window has the focus. |
| `COLORS` | The widgets' colours. |

Input: a left click (press and release) over a mod's region is taken from the game: the UI manager's event handler
(0x00757840) reports it handled, so neither the game's windows nor the world see it, and the release that follows
goes to the same region. Regions on the world and hud layers take nothing while the escape menu or a message box is
up. Hover is decided against the previous frame's regions (the topmost under the cursor), so an open dropdown hides
what lies below it. Keyboard input is only `key_pressed` for now (no text fields).

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
| `selftest` | An automation run: loads a savegame, checks the hero, gold, nearby creatures, types, optionally a spawn, writes `GearAutomation\selftest.json` and `types.txt`, screenshots, quits with 0 or 1. |

## Automation

For self tests and scripted runs without a player. Off unless the game starts with `-gear-auto` (or
`SACREDGEAR_AUTO=1` in its environment). Then:

| Option | |
|---|---|
| `-gear-load=N` (`SACREDGEAR_LOAD`) | Loads savegame N (`Save\GAMEnn.PAK`, 0 the quicksave) as soon as the main menu is up, the way the menu's savegame window does it. |
| `-gear-mods=a,b` (`SACREDGEAR_MODS`) | Loads only these mods. |

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
| `screenshot(name)`, `write(name, text)` | Files in `<game>\GearAutomation` (name: letters, digits, `_ - .`). |
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

Example, from a shell: `Sacred.exe -gear-auto -gear-load=1 -gear-mods=selftest`, then read
`GearAutomation\selftest.json` and the exit code.

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
| SacredBild (optional) | `ddraw.dll` exports `SacredBild_UiPushFrame(window, confine)`, `SacredBild_UiPopFrame()` and `SacredBild_UiScreenBounds(float[4])`; Gear draws a command with `window` in that window's UI frame and the others in the unconfined canvas. Without them (no SacredBild) the layout is drawn as the game draws it. |
| Items | An item is a `cItemBase` (RTTI type descriptor 0x0094EBC0). Its +0x118 holds a copy of its item record's data; the dword +0x11C: class mask (bits 0-5 Seraphim, Gladiator, Battle Mage, Dark Elf, Wood Elf, Vampiress; 0x400000 Dwarf; 0x800000 Daemon; 0 or 0xC0003F every class) and rarity (bits 16-19). Item records: `*(typeManager + 0x563004) + index * 0x102` (index: the type record's +0x18; count at +0x563000), +0x23 nonzero: set item. Object flags +0x14: 0x02 not in the world (inventory, being picked up), 0x10 freed. Type record +0x00 bit 24: quest item. Gold: types 0x140C-0x140F. Potions: 0x13C0-0x13C8 health / mana / stamina bottles (three sizes each); 0x1411, 0x1416, 0x1430: large, medium, small coloured potions (black, blue, green, red, yellow); which colour does what is not checked. The hero's class is its type (1..9, 7 unused). |
| Pickup | `cCreature::pickupItem` 0x00558080 (thiscall (creature, item id) -> 1 / 0, ret 4) is the only code that moves a world item to a creature: the click pickup (0x0053CEB0), the collect behaviour (0x004ECF69, 0x004ED095) and the host's message 0x20C call it and ignore the result. Gear returns 0 there for a blocked item and the local hero. The collect behaviour's filter 0x004EC330 (thiscall (creature, cObject*) -> bool; settings.cfg PICKUPAUTO) returns false for them. A network client sends message 0x141 instead (0x0053A170). Item label colour: 0x005C13C0 (thiscall (item, record or 0) -> ARGB): set green 0xFF15FF25; rarity 0-4 0xFFEFEFEF, 5-11 0xFFA0A0FF, 12-14 0xFFF0E040, 15 0xFFC19A33; class-restricted items take their class's colour unless UNIQUE_COLOR is on. |