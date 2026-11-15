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

Events: `'load'` (all mods loaded), `'frame'` (`dt`, every in-game frame), `'session.start'` (`hero`, a creature
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
only; other DLLs in the process (SacredBild among them) still see the real foreground window and keys.

Example, from a shell: `Sacred.exe -gear-auto -gear-load=1 -gear-mods=selftest`, then read
`GearAutomation\selftest.json` and the exit code.

## Compatibility

- No mods: Sacred Gear installs one hook (the engine's per-frame zoom update, which it calls unchanged) and does
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
