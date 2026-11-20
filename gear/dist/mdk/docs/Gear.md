# Sacred Gear

Sacred Gear is a mod kit for **Sacred Gold**: mods are Lua (LuaJIT) scripts that read the game's data and world, react
to its events, draw their own UI and change the game. It is a DLL of its own, `SacredGear.dll`, built from this
repository next to SacredBild but independent of it: it works with SacredBild, without it, and with any DLL loader.

- **This page**: installing, writing a mod, the file overlay, the mod manager, automation and the examples.
- **[GearApi.md](GearApi.md)**: the reference of every function, option, field and event, generated from the LuaLS
  annotations in `gear/meta/` (which also give editors completion and checks:
  [Editor support](GearApi.md#editor-support-lua-language-server)).
- **[The MDK](https://github.com/kirides/sacred-bild/blob/main/gear/mdk/README.md)** (`sacred-gear-mdk.zip` in every release): the annotations, both documents, a mod
  template and examples, ready to open in VS Code.
- **[sacred-gear-mods](https://github.com/kirides/sacred-gear-mods)**: the mods players use (loot filter, layout editor,
  shared stash, autosave, server mod clients, ...), each with its own page.
- **[Custom classes](https://github.com/kirides/sacred-bild/blob/main/docs/CustomClasses.md)**: hero classes of your own, ability copies, the class and motion hooks (`gear.classes`, `gear.models`).
- **[Sacred Gear in network games](https://github.com/kirides/sacred-bild/blob/main/docs/GearMultiplayer.md)**: how mods work in LAN and online games.

Supported executable: the **English** GOG `Sacred.exe` (PE timestamp `0x452F85C7`). Sacred Gear finds the game's code
by byte signatures, like SacredBild, so other builds with the same code may work; if a signature isn't found, Sacred
Gear logs it and leaves that feature off.

## Install

Sacred Gear needs something to load it into the game:

| How | What to do |
|---|---|
| With SacredBild | Put `SacredGear.dll` into the game folder. SacredBild loads it at startup. |
| With an ASI loader (e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll` or `winmm.dll`) | Rename it to `SacredGear.asi` and put it where the loader looks for plugins (the game folder or its `scripts`/`plugins` folder). |
| With an injector | Inject `SacredGear.dll` into `sacred.exe`. Early is better: injected after a game started, the mods load with the next in-game frame. |

Mods go into the **`mods`** folder of the game folder, one folder per mod. Without mods Sacred Gear changes nothing.
It logs to `SacredGear.log` in the game folder (the two runs before are kept as `.prev.log` and `.prev2.log`). If two
copies end up in the game (`SacredGear.asi` through a loader and `SacredGear.dll` through SacredBild), the first one
that loads is active and the other does nothing.

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
    author = 'You',
    description = 'Says hello in the log.',
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

`gear/examples/hello` is a complete example; the MDK's `tools/new-mod.ps1` makes a new mod from a template.

### mod.lua

| Field | |
|---|---|
| `id` | Required: letters, digits, `_` and `-`; unique among the installed mods |
| `name`, `version` | Shown in the Mods screen and the log; `version` is part of the mod set |
| `description`, `author` | Shown in the Mods screen |
| `priority` | Lower loads first (default 0); ties load by id |
| `requires` | Ids that must load first; the mod is skipped if one is missing (and off while one of them is off) |
| `enabled` | `false`: off until the player switches it on in the Mods screen (a default, not a lock) |
| `settings`, `keys` | Declared settings and key binds: the [Mod manager](#mod-manager) builds their pages |
| `config` | Default values for `gear.config`; `config.lua` in the mod's folder (a table, values only) overrides them. For what has no screen (lists, self test switches). A declared setting with the same id takes its default from there. |
| `pipes` | Names of the named pipes the mod may open (`gear.pipe.open`) |
| `automation` | `true`: the mod gets `gear.auto` when the game runs with `-gear-auto` |

### Events

`gear.on(event, fn)` registers a handler and returns a handle for `gear.off`. The main events: `load` (all mods
loaded), `frame` (`dt`, every in-game frame), `draw` (after the frame handlers: the place to draw UI),
`session.start` (`hero`: a game with a hero runs) and `session.stop`, `settings.changed` (`id, value`), `unload` (the
player switched the mod off: clean up), `savegame.changed` (a savegame was loaded or a new game started), and
`game.saved` / `game.save_failed` / `game.saved_other`. Engine hooks such as `damage`, `kill`, `drops.roll` or
`creature.ability` let a handler change what the engine does. All of them: [GearEventName](GearApi.md#geareventname).

Custom events have a dot in their name (`twitch.command`; `session.*`, `gear.*` and `pipe.*` are reserved) and are
fired with `gear.emit`. A mod that publishes events for other mods declares them with `gear.events.declare`
([Hooks between mods](GearApi.md#hooks-between-mods-gearevents)).

### When mods run

The mods load with the first in-game frame (with [automation](#automation) on: in the main menu already). All mod
code runs on one thread at a time: in game the engine thread, between the game's update and the drawing of the frame,
so handlers see the frame's state, never a half-updated one, and need no locks. Timers (`gear.after`, `gear.every`)
count real seconds while the game runs frames; `gear.task(fn)` runs code that may wait (`gear.wait`,
`gear.wait_until`).

### What a mod gets

- **Libraries:** `string`, `table`, `math`, `bit`, `coroutine`, and from `os` only `clock`, `time`, `date` and
  `difftime`. There is no `io`, `os.execute`, `debug`, `load`/`loadstring`/`dofile`, `jit` or `ffi`: a mod reads only
  its own files and cannot reach the rest of the computer. Each mod has its own globals.
- **Its own files:** `require(name)` runs `name.lua` of the mod's folder once (`a.b` is `a\b.lua`); `gear.read(name)`
  and `gear.save(name, text)` read and write files directly in the mod's folder (`*.json`, `*.txt` or `*.lua` other
  than `mod.lua` and `main.lua`, at most 8 MB, written atomically).
- **Stored data:** player settings through `gear.settings` (the Mods screen shows them), the mod's own data with
  `gear.store.file`, and data that belongs to one savegame with `gear.store.game` (kept per savegame, so it does not
  leak between characters; see [gear.savegame](GearApi.md#per-savegame-data-gearsavegame)).
- **The game:** data tables, the world, items and inventories, saving, the party, drawing and widgets, drops, chat,
  network messages. The [reference](GearApi.md) lists everything by area.

Rules that hold across the API:

- **Single player and network games.** Calls that change the world (`gear.world.spawn`, `teleport`, boss modifiers,
  the party) work only in single player, `gear.world.can_change()` says when; in a network game they return nil and a
  message. Gold and the inventory calls also work in a network game for the local hero's own gold, backpack and the
  chest, through the engine's own announcing calls (`gear.inv.can_change()`). Pickup rules hold for the local hero in
  single player and for the host; a network client's pickups go through the host.
- **Drawing** is immediate mode in the UI's 1024x768 layout: a mod draws its overlay every frame in a `draw` handler,
  on the `world`, `hud` or `top` layer, optionally inside a game window (`opts.window`) so it moves with it. With
  SacredBild the layout is scaled to the screen and the game's windows can be moved; without it the layout is the
  screen. See [Drawing](GearApi.md#drawing-gearui).
- **The game's own look:** `gear.ui.native.frame(x, y, w, h, opts)` draws the border and fill of the game's item tooltip
  (dark metal bars, ornaments in the corners; built from the game's UI images, native size at the corners, stretched
  bars), so a panel of a mod can look like the game's tooltip while its text stays the mod's own. `gear.ui.native.insets()`
  tells how much room the border takes. `gear.ui.tooltip_anchor(w, h)` gives the position that attaches such a panel flush
  to the right of the game's item tooltip (on its left near the screen's edge; `gear.ui.hovered_item()` reports the
  tooltip's visible rectangle in the layout `gear.ui` draws in). Further pieces (rings, bars) are added as styles to the same table. See
  [The game's own look](GearApi.md#the-games-own-look-gearuinative).
- **HTML/CSS documents (prototype):** `gear.ui.rml` loads a `.rml` document and its `.rcss` styles from the mod's folder
  ([RmlUi](https://mikke89.github.io/RmlUiDoc/): `div`, `button`, `input`, flexbox, `:hover`, animations) and draws it every frame
  with `doc:draw()`; `doc:on('#ok', 'click', fn)` calls Lua, `doc:text`, `doc:class`, `doc:style` and `doc:value` change the
  document. Every button, link and input is a pad stop, a focused text field takes the keyboard, the default font is Segoe UI, and the
  game's own UI images are available as `gear:image/<id>`, CSS `transform` works. No JavaScript, no filters or drop shadows yet. See
  [HTML/CSS documents](GearApi.md#htmlcss-documents-gearuirml) and `gear/examples/rmldemo`.
- **Item data:** `gear.inv.values(item)` decodes an item's resolved value record (level, requirements, damage, attack, defense,
  protection, the eight bonus slots with code, attribute and stored value, and the records its sockets merged in), and
  `gear.inv.bonus_text(item, entry, slot)` gives the game's own tooltip line for one bonus slot (the engine's routine, so the
  game's language, colours and numbers). Numbers the engine derives (the "expected" damage and protection of the hero, the
  movement speed, the price, set bonuses) are not in the record: read them from `gear.ui.item_tooltip_text`.
- **Mouse and keys:** a click over a mod's widget or panel is taken from the game, and the game's windows do not see
  the cursor there. Mods must not take the game's own keys (see [Mod manager](#mod-manager)); `gear.input` remaps the
  game's keys instead.

### Errors

An error in a mod's code is logged with its traceback and does not reach the game. A mod whose handlers raised ten
errors is switched off until it is switched on again in the Mods screen. A handler that runs too long (an endless loop:
0.25 s for a frame's handler, 5 s for loading) is stopped with an error; LuaJIT checks this only in interpreted code, so
a loop the JIT compiled may not be caught. The game's functions that Gear calls for a mod run under a fault guard: a
call that faults returns nil and a message instead of crashing the game.

### Names and IDs

The game's texts follow the player's language, so a name is never a key. A mod that keeps something between runs (its
settings, a file, a savegame's data, a message to a server) follows these rules:

- **Save ids.** An object type is its number (`creature:type()`, `item.type`, the `type` of `drops.roll` and the kill event),
  stable in every language and install; an ability, a region or a sector likewise. Save `{type = 38}`, not `"Orkkrieger"` and not
  `"ORC_WARRIOR.GRN"`. A *pattern* that is meant to match a family ("any creature whose model name has the word orc") is not a
  name: it may be saved as the word, but then the mod must say so in its docs and never show it as a name.
- **Show names.** What the player reads comes from `gear.data.object_types:label(id)` (the localized name, else the internal
  one, else `type N`), `creature:display_name()` or the kill event's `display_name`; items also from `gear.ui.item_tooltip_text`.
  A name saved next to an id (a log entry's text) is a snapshot: draw the label of the id when there is one.
- **Search by name.** A picker searches the localized names and the internal words: `gear.lookup.pick('creature_type')`,
  `'creature'` (families) and `'item'` do. Hand the result's `id` on, not its name.
- **Migrate on load.** Data from an older version may hold names. Turn each into an id with `object_types:resolve(name)` (a
  number, an internal name or a localized creature name), write the new form with the next save, and keep an entry that
  resolves to nothing as it is, so it survives an update that adds the type. The reader accepts both forms for good.
- **Protocols.** What a server understands stays what it understands: add the id next to the name instead of replacing it.

## Replacing game files (file overlay)

A mod can ship data files that the game opens instead of its own. `mods/<id>/overlay.txt` has one rule per line
(`#` starts a comment):

- `bin\balance.bin = data\balance.bin`: the game's open of that path (relative to the game folder) gets the mod's file
  if it exists.
- `saves = save`: the game's `Save\` folder is the mod's `save\` folder; the vanilla saves are neither read nor written.
- `bin\balance.bin = patch:balance.lua`: the game's file with the changes of a patch file (below).

The rules are read when Sacred Gear starts, because the game opens its data files before any Lua runs: for the mods
the run uses (`-gear-mods=a,b`, else all) that are switched on. A mod that is off gets no rules. `gear.files.overlay()`,
`resolve(path)` and `served(path)` show what applies and what the game really opened through it.

**The gameserver.** A hosted or LAN game runs `GameServer.exe`, which reads `bin\balance.bin` and the scripts itself and
compares them with the client's. When the game starts `GameServer.exe` and an enabled mod has file rules, Sacred Gear
starts it with `SacredGear.dll` injected and passes it the ids of those mods (environment variable
`SACREDGEAR_OVERLAY_MODS`). In the server Sacred Gear is headless: it installs the same file rules (not the save
redirect) and logs to `SacredGear-server.log`. So a mod's balance works for LAN and hosted games. Without mods with
file rules there is no injection. Tests: `tools\server-overlay-test.ps1 -GameDir <copy of the game folder>` starts the
server alone; `tools\lan-host-test.ps1 -GameDir <copy> -Patch` hosts a real LAN game with a changed `balance.bin`
(without the server overlay the game shows "IP: Kann nicht verbinden! CODE -10021").

**Patch files** ship only what differs from the game's file. A patch is a Lua data file, read by a plain Lua state
without libraries (so it can only be data). The first open of the file builds the patched copy in the mod's `cache\`
folder (rebuilt when the patch or the original changes) and serves that; a game file of another size or CRC-32 than
the patch was made for is served unpatched and logged.

- **Bytes:** `return {size = n, crc32 = 0x..., ops = {{offset, 'hex'[, replaced]}, ...}}`: offsets in the original,
  ascending; the hex bytes replace `replaced` bytes (default: as many as there are).
- **`kind = 'words'`**, for tables of 32-bit words like `balance.bin`: `ops = {{first word index, {value, ...}}, ...}`
  (a value is an unsigned integer or `{f = float}`), and for tables of records `records = {first = word index of
  record 0, words = words per record, fields = {name = word in the record}}` with `edits = {{record = n | records =
  {from, to}, field = value, ...}}` (also as `sections = {{records, edits}, ...}` next to `ops`).
- **`kind = 'texts'`**, for a language's `global.res`: `set = {[text id] = 'text'}`, `remove = {ids}`.
- **`kind = 'script'`**, for the compiled scripts `FunkCode.bin` / `StartCode.bin` (streams of instructions `u16 opcode,
  u16 length, operands`; `FunkCode.bin` is cut into named functions by `Vectoren.bin`, 84-byte records `char name[64],
  i32 start, i32 length, ...`): `file = 'funk' | 'start'`, `edits = {...}`, no `size`/`crc32`. Edits find their
  instruction themselves:
  - `{fn = 'Sector88067Enter', op = OPCODE.FILL_SECTOR, sector = {88, 67}, [set_sector = {x, y},] spawn = {old = {...},
    new = {{creature = CREATURE.X, weight = 10, group = 1}, ...}}}`: the `FillSector` instruction of that function for
    that sector (the spawn table that ends it, 13 bytes per entry after a 40-byte header) and/or the sector it fills;
    `ins = n` only if the sector alone is not unique;
  - `{op = OPCODE.SET_VAR, var = 'Name', old = n, new = n}`: the `SetVar` instruction of that variable (`StartCode.bin`);
  - `{fn, op, ins = n, at, old = 'hex', new = 'hex'}`: a byte range in the n-th instruction.

  The function and the instruction must be unique, the opcode must match and `old` must be what the file holds, or the
  whole file is served unpatched and the reason is logged. One patch file can serve many game files (all hero types
  share the same library code). Format notes: sacred-gear-reborn-decomp's `docs/file-formats.md`.
- **Names:** every `.lua` file in the mod's `constants/` folder returns a table of names to numbers and is a global of
  its upper-case name in the patch files (`constants/creature.lua` -> `CREATURE.WOLF`; also `ITEM`, `OPCODE`); the
  cache is rebuilt when they change.

A mod that overlays `balance.bin` without the gameserver overlay is for single player only: LAN games need the client
and the gameserver to read the same `balance.bin`.

## Mod manager

The **Mods** screen is built into Sacred Gear (`gear/lua/modmanager.lua`, loaded as the mod `gear-mods`). It lists every
installed mod with its name, version, author, description, an On/Off switch and its state (switched off after ten
errors, off because a mod it requires is off, a key shared with another mod). Each mod has a **Settings** page (made
from its declared settings, plus a page of its own if it registers one) and a **Keys** page.

**Players.** Press **F11** in game, or click **Mods** under the game's escape menu. **Esc**, the Close button, the X or
F11 close it; while it is open the game gets no keys, so typing into a text setting does not fire hotkeys. Every choice
is saved at once in `mods\gear_mods.json` (On/Off, settings and key binds of all mods; written atomically; a file that
cannot be read is kept as `gear_mods.json.bak` and the defaults apply).

- A mod switched off is unloaded at once: it gets `unload`, then its handlers, timers, tasks, pipes, key binds, pickup
  rules and widgets are dropped (what it did to the world stays until the game restarts). Switching it on loads it
  fresh (`load`, and `session.start` when a game runs). Both take effect at the start of the next tick.
- A mod switched on brings the mods it requires; mods that require a switched-off mod are off while it is.
- A mod that failed (ten errors) is switched off and on to try again.
- **Reset to defaults** on a mod's page resets its settings, **Reset** one setting, **Default** one key.
- A key that two mods use shows a conflict on both mods' rows (both still fire; rebind one). A bind on one of the
  game's own keys (digits, Q W E R B, A, I, F, C, L, M, Tab, O, S, H, N, P, Space, Esc, + and -, F8 quick load, F9 quick
  save; the game reacts to them with any modifier held) shows a conflict with "The game".

Default hotkeys: F11 the Mods screen, F4 the widget gallery of `uidemo`; the mods of sacred-gear-mods use F2, F3, F5,
F6 and Ctrl with letters, never F7, F8 or F9
([the full table](https://github.com/kirides/sacred-gear-mods/blob/main/docs/Hotkeys.md#hotkeys-and-the-clashes-in-the-defaults)).

**Mod authors.** Declare settings and keys in `mod.lua`:

```lua
return {
    id = 'mymod', name = 'My mod', version = '1.0', author = 'Me',
    description = 'Does something useful.',
    settings = {
        {id = 'enabled', type = 'bool',   label = 'Active', default = true, tooltip = 'Turns the effect off'},
        {id = 'radius',  type = 'number', label = 'Radius (tiles)', min = 5, max = 100, step = 5, default = 40,
         section = 'Range'},
        {id = 'mode',    type = 'choice', label = 'Mode', default = 'near',
         choices = {'near', 'far', {value = 'both', label = 'Both of them'}}},
        {id = 'tag',     type = 'text',   label = 'Label', default = 'x', max_length = 20, restart = true},
        {id = 'tint',    type = 'color',  label = 'Color', default = 0xFF3080C0},
    },
    keys = {
        {id = 'panel', label = 'Open the panel', default = 'f5'},       -- 'ctrl+shift+k' works too
    },
}
```

```lua
local S = gear.settings
local radius = S.get('radius')                         -- the player's value, else config / config.lua, else default
gear.on('settings.changed', function(id, value)        -- from the Mods screen or from gear.settings.set
    if id == 'radius' then radius = value end
end)
gear.keys.on('panel', function() gear.ui.show_panel('p', true) end)   -- once per press, exact modifiers
gear.on('unload', function() end)                      -- the player switched the mod off: clean up
gear.mods.page(function(x, y, w, h)                    -- an extra "More" tab with widgets of the mod's own
    gear.ui.label('hint', 'Anything gear.ui can draw', x, y, {})
    return 24
end)
```

| Setting type | Widget | Value |
|---|---|---|
| `bool` | checkbox | `true`, `false` |
| `number` | slider | a number from `min` to `max` (default 0..100) on the grid of `step` (default 1) |
| `choice` | dropdown | one of `choices` (strings, numbers or `{value, label}`) |
| `text` | text field | a string of at most `max_length` (default 120) characters |
| `color` | swatch and four sliders | `0xAARRGGBB` (set it from a number, `{r, g, b, a}` or `'#AARRGGBB'`) |

A value that no longer fits (a removed choice, a number out of range) falls back to `config`, then to the default.
`restart = true` marks a setting the mod reads only at start (the screen says "after restart"). `gear.settings.migrate`
reads an old settings file of the mod once. A declared key is bound to its `default` until the player changes it
(`''`: unbound); bindings are lower case, modifiers first (`ctrl+shift+m`). Hotkeys stay quiet while the Mods screen is
open. Pick a default that is free (the table above). The rest: [Settings, key binds and the mod
manager](GearApi.md#settings-key-binds-and-the-mod-manager); remapping the game's own keys:
[gear.input](GearApi.md#remapping-the-games-keys-gearinput).

Self tests: `gear/examples/modmanagertest` clicks through the real screen in a game; `gear/tests/kernel_test.lua` covers
the kernel side (persistence, broken files, switching mods live, dependencies, key binds and conflicts).

## Online play: game mode and realms

`gear.mp.mode()` tells the kind of game: `'single'`, `'lan'`, `'open'` or `'closed'` (Closed Internet: the heroes belong
to lobby accounts and the server keeps them). It is read from the engine, default-deny: whatever cannot be proven Open
Net counts as `'closed'`. Mods use it to keep trust-based features (a local shared stash) out of Closed Net.

`gear.realm` chooses the server for online play (register and login), so several servers can be used without editing
`Settings.cfg`. Sacred Gear puts the chosen realm's host (and port) in place of `NETWORK_LOBBY` / `NETWORK_LOBBYPORT`
when the multiplayer menu opens **Closed Internet** or **Open Internet**; LAN is not touched and `Settings.cfg` keeps the
game's own values. The choice is in `mods\gear_realm.txt`, the list of realms in `mods\gear_realms.json`
(`{"realms": [{"id", "name", "host", "port"}], "active": ...}`). A realm applies from the next opening of those menus.
The `realms` mod (sacred-gear-mods) is the picker. A realm decides where the player's password goes: use only realms you
trust.

Mods in network games, the mod channel and named net channels: [Sacred Gear in network games](https://github.com/kirides/sacred-bild/blob/main/docs/GearMultiplayer.md).

## Automation

For self tests and scripted runs without a player. Off unless the game starts with `-gear-auto` (or
`SACREDGEAR_AUTO=1` in its environment). Then the mods load in the main menu and get the `idle` event there, and mods
with `automation = true` get [`gear.auto`](GearApi.md#automation-gearauto) (raw and task-level input, UI state,
savegame loading, screenshots, files, quitting).

| Option | |
|---|---|
| `-gear-load=N` (`SACREDGEAR_LOAD`) | Loads savegame N (`Save\GAMEnn.PAK`, 0 the quicksave; through the file overlay) as soon as the main menu is up. A missing savegame ends the run with exit code 4 (the game itself would crash). |
| `-gear-mods=a,b` (`SACREDGEAR_MODS`) | Loads only these mods. |
| `-gear-instance=NAME` (`SACREDGEAR_INSTANCE`) | Names the run, default `pid<PID>`. See [Parallel runs](#parallel-runs). |
| `-gear-window=WxH[+X+Y]` (`SACREDGEAR_WINDOW`) | SacredBild only: a framed window with that client size (default 1600x900, at least 1024x768) in the bottom-right of the primary monitor's work area, or at screen pixel X,Y. `[Display] Width`, `Height` and `Borderless` are ignored. |
| `-gear-timeout=SECONDS` (`SACREDGEAR_TIMEOUT`) | The watchdog: default 300, 0 = off. A run that takes longer, a game window that does not answer or an in-game run without a frame for 60 s quits with exit code 3. |
| `-gear-shared-input` (`SACREDGEAR_SHARED_INPUT=1`) | Turns the input isolation off (below). |

Example: `Sacred.exe -gear-auto -gear-load=1 -gear-mods=selftest -gear-instance=a`, then read
`GearAutomation\a\selftest.json` and the exit code.

**Isolation.** A test can run while you use the PC: the game never comes to the front or takes the focus (its calls to
`SetForegroundWindow`, `SetFocus`, `SetCapture`, `ShowWindow`, `SetWindowPos` and so on become non-activating ones),
ignores the real mouse and keyboard (their messages are dropped; `GetCursorPos`, `GetAsyncKeyState`, `GetKeyState` and
`GetKeyboardState` report only injected input; `SetCursorPos` and `ClipCursor` never touch the real cursor), and
believes it is the active foreground window (a virtual one: `GetForegroundWindow`, `GetFocus` and `GetActiveWindow`
answer with the window it asked to activate). Injected input reaches it through its own message queue; characters come
from `TranslateMessage` of the injected key messages. These are import hooks on `sacred.exe` only. SacredBild asks
`SacredGear_IsolatedForeground()` (an optional export) and counts the game as active then. Run the game in a window: an
exclusive fullscreen display takes the screen whatever Gear does. With `-gear-shared-input` the game takes real input
too, and the cursor is virtual only until the real mouse moves.

**Screenshots** (`gear.auto.screenshot`) read the game's back buffer in-process on the thread that renders (read-only
lock of the DirectDraw surface; vanilla ddraw and SacredBild's backend, 16 and 32 bit), so they need neither focus nor a
visible window. In the menus the back buffer is empty between some frames, so it asks again (up to 40 tries) and falls
back to the primary surface.

### Parallel runs

Every automation run is an *instance* with a name: `-gear-instance=NAME` (letters, digits, `_ - .`, at most 48
characters, anything else becomes `_`; default `pid<PID>`, so runs without a name never meet either). What a name
changes:

- **The single-instance check.** Sacred Gear names the game's mutex `SACRED_INSTANCE-gear-NAME`, so runs with
  different names never collide, nor with a player's game; the same name twice still does ("already running").
  Automation runs should not host LAN games in parallel (the game finds its gameserver by window class).
- **The window title** gets the suffix ` [gear:NAME]`.
- **`<game>\GearAutomation\NAME\`** holds everything the run writes: `screenshot()` and `write()` files,
  `SacredGear.log`, and with SacredBild `SacredBild.log` and crash dumps; its own `gear_mods.json` (starting empty, so a
  test never touches the player's mod settings); and `run.json`.
- **`run.json`** is written at the start (`{"instance", "pid", "command_line", "started"}`) and again at the end with
  `"exit_code"`, `"ended"` and `"reason"`: `quit` (`gear.auto.quit`), `watchdog` (exit code 3) or `exit` (the game ended
  itself). Without `ended` the run is still going, or was killed or crashed. The `pid` is the real process of that run.

Still shared between parallel runs, because the game or the game folder owns it: `Save` (a run that saves overwrites
the slot other runs load: load only, or use a copy of the folder), `Settings.cfg`, the game's logs and `bin\sg*.bin`,
`SacredBild.ini`, `SacredBild-profile.txt`, the mods' own files (`mods\gear_layout.json`, a mod's data) and the DLLs
(copy `ddraw.dll` and `SacredGear.dll` once, before starting the runs). Runs share one CPU and GPU: frame times in a
self test mean little, results do.

**A second game by hand.** `-gear-instance=NAME` without `-gear-auto` starts a *manual instance*, for testing LAN or
online play with two clients on one PC: `sacred.exe -gear-instance=two -gear-window=1600x900` (or
`tools\second-instance.ps1 -Game <folder>`). It gets the mutex, the title suffix and its own log and `run.json` folder,
nothing else: real input, no automation, the player's own `gear_mods.json`. Start the first game normally and the second
with a name.

## Examples

`gear/examples` holds small feature examples and the automation tests; the mods players use are in
[sacred-gear-mods](https://github.com/kirides/sacred-gear-mods). Tests that save or leave items in the world need a copy
of the game folder.

| Mod | |
|---|---|
| `hello` | The smallest mod: events and the creature types. |
| `uidemo` | `gear.ui`: a hero panel, a widget gallery (F4) with every widget, images in the three fit modes, the fonts, tooltips, notes on the world and top layers and in the inventory's frame; with `-gear-auto` a self test that clicks every widget. |
| `rmldemo` | `gear.ui.rml`: a window written as an RmlUi document (HTML and CSS): buttons and a text field that call Lua, a bar that follows the hero's health, the game's tooltip corner ornaments as images (F6 shows and hides it). A prototype: not yet run in game. |
| `theme` | The shared RmlUi look: `mod.lua` has `theme = 'theme.rcss'`, and Gear puts that stylesheet in front of the styles of every RmlUi document of every mod (classes `window`, `row`, `primary` / `danger` buttons, `field`, `list`, `bar`, `padkey` ...), so mods only override what is special. Mods opt out with `{theme = false}`. A prototype: not yet run in game. |
| `nativemenu` | The game menu (Esc) as an RmlUi screen: `gear.ui.replace_window`, the game's own entries through `gear.ui.menu_entries` / `menu_click`, and the items other mods add with `gear.menu.add_item` (the Mods manager's "Mods" is one); off by default, with it off nothing changes. A prototype: not yet run in game. |
| `hooks` | Reacts to the hooks other mods publish and publishes one of its own. |
| `saveinfo` | Loads a savegame and writes where the hero stands and what is around. |
| `selftest` | Loads a savegame, checks the hero, gold, nearby creatures, types, optionally a spawn, writes `selftest.json` and `types.txt`, quits with 0 or 1. |
| `itemicontest` | `gear.ui.item_icon` by type id and from live items. |
| `modmanagertest`, `mmtest_aux`, `mmtest_dep` | The Mods screen's in-game self test, with a helper mod and a mod that requires it. |
| `scrolltest`, `tooltiptest`, `uitest`, `chesttest`, `padnavtest` | Scroll areas and dropdowns; tooltips at the screen's edges; the UI in game; shop windows at a fixed UI scale; the controller path of the mod UI. |
| `tabletest` | `gear.ui.table`: tables of labels, dropdowns, checkboxes, sliders and buttons in columns, a scrolling table, an auto column, borders, row and column spans and alignments; the self test checks the geometry and clicks the cells (24 checks). |
| `partytest` | `gear.party`: recruit, orders, switching control, shared experience, save and load with a companion (`-gear-load=N`, a savegame with an enemy nearby; writes savegame 63). |
| `classtest` | `gear.classes`, `gear.models`, `gear.text.add`, `creature:learn` / `forget`: catalog and tiers, hero skills, an ability copy used by a recruited Gladiator; 26 checks. `classspike` is the first run of the class and motion hooks. See [Custom classes](https://github.com/kirides/sacred-bild/blob/main/docs/CustomClasses.md). |
| `droptest` | The drop hooks: the loot function run a few hundred times per phase with changed chances, rarity weights, a veto, a replaced item and the generators; 31 checks. See [Gear drops](https://github.com/kirides/sacred-bild/blob/main/docs/GearDrops.md). |
| `bosstest` | Creature modifiers: health, damage dealt and taken, reflection, regeneration, spawn with modifiers, save and load with a boss (a savegame with a hostile creature 1266; 24 checks). |
| `savegametest` | Per-savegame data (`gear.store.game`) across saves and loads. |
| `mptest`, `netopstest` | The mod channel and `gear.net` (loopback, or a hosted LAN game; `netopstest` against the stock or the SacredEngineRemake server). |
| `realmtest`, `closedtest` | `gear.realm` in the menus; Closed Internet end to end against a test lobby. |
| `servertest`, `lanhosttest` | The gameserver's file overlay; hosting a LAN game through the menus. |

## Compatibility

- Without mods Sacred Gear installs its hooks, calls the game's code unchanged through them and does nothing else:
  savegames, multiplayer and the game's behaviour are the vanilla ones. Automation's import and message hooks exist
  only when it is on.
- Sacred Gear never writes into the game's savegames. Mod data kept per savegame goes into the mod's own folder, so
  removing a mod never breaks a save.
- SacredBild and Sacred Gear never patch the same bytes (`tools/check_gear_overlap.py`), so they work in either load
  order.
- In network games every player needs the same mods. Sacred Gear logs a hash of the loaded mods (ids and versions) as
  "mod set".

Building and changing Sacred Gear: [Development](https://github.com/kirides/sacred-bild/blob/main/docs/Development.md#sacred-gear); the engine facts it relies on:
[RE notes](https://github.com/kirides/sacred-bild/blob/main/docs/RE_NOTES.md#objects-items-saving-and-the-ui-pass-sacred-gear).
