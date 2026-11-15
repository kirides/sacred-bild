# Sacred Gear

Sacred Gear is a mod kit for **Sacred Gold**: mods written in Lua (LuaJIT) that read the game's own data tables,
react to its events and, in later versions, change and extend them. It is a DLL of its own, `SacredGear.dll`, built
from this repository next to SacredBild but independent of it: it works with SacredBild, without it, and with any
other DLL loader.

Status: **milestone 1**, the skeleton. Mods load and run, get a per-frame event and can read the creature types.
Nothing a mod can do yet changes the game. See [Roadmap](#roadmap).

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

The mods load with the first in-game frame (M1; earlier loading follows with the data milestone). All mod code runs
on one thread, the game's engine thread, between the game's update and the drawing of the frame: handlers see the
frame's state, never a half-updated one, and need no locks.

### Lua API (M1)

| | |
|---|---|
| `gear.on(event, fn)` | Calls `fn` for `event`; returns a handle. Events: `'load'` (all mods loaded), `'frame'` (`dt`, in game, once per frame). |
| `gear.off(handle)` | Removes a handler. |
| `gear.log(...)`, `print(...)` | A line in `SacredGear.log`, prefixed with the mod's id. |
| `gear.time()` | Seconds, high resolution, from an arbitrary start. |
| `gear.version` | Sacred Gear's version. |
| `gear.mod` | This mod's `id`, `name`, `version` and `dir` (its folder name). |
| `require(name)` | Runs `name.lua` of this mod's folder once (`a.b` is `a\b.lua`) and returns its result. |
| `gear.data.creature_types` | The creature types the game loaded from `Pak\Creature.pak`, read only: `:count()`, `:stride()` (record size, 0x56), `:get(i)` (record `i`, from 0 like the game's own index, or nil). A record reads fields by byte offset: `:u8(off)`, `:i8`, `:u16`, `:i16`, `:u32`, `:i32`, `:f32`; nil if out of range. Field names follow once the layout is mapped. |

The standard libraries a mod gets: `string`, `table`, `math`, `bit`, `coroutine`, and from `os` only `clock`, `time`,
`date` and `difftime`. There is no `io`, `os.execute`, `debug`, `load`/`loadstring`/`dofile`, `jit` or `ffi`: a mod
reads only its own files (through `require`) and cannot reach the rest of the computer. Each mod has its own globals.

### Errors

An error in a mod's code is logged with its traceback and does not reach the game. A mod whose handlers raised ten
errors is switched off until the game restarts. A handler that runs too long (an endless loop: 0.25 s for a frame's
handler, 5 s for loading) is stopped with an error; LuaJIT checks this only in interpreted code, so a loop the JIT
compiled may not be caught.

## Compatibility

- No mods: Sacred Gear installs one hook (the engine's per-frame zoom update, which it calls unchanged) and does
  nothing else. Savegames, multiplayer and the game's behavior are the vanilla ones.
- Sacred Gear never stores anything in the game's savegames. State that mods will keep per savegame (a later
  milestone) goes into files of their own next to the savegame, so removing a mod never breaks a save.
- SacredBild and Sacred Gear never patch the same bytes: `tools/check_gear_overlap.py` checks every signature window
  of the one against every one of the other, so they work in either load order.
- Multiplayer is not supported by mods yet. Sacred Gear logs a hash of the loaded mods (ids and versions) as "mod
  set"; a later milestone sends it in the connection handshake so that a game only accepts peers with the same mods.

## Roadmap

| Milestone | Contents |
|---|---|
| M1 (this) | `SacredGear.dll`, any loader, LuaJIT host, `mods` folder, `load` and `frame` events, creature types read only |
| M2: data | Item, weapon and balance tables with named fields (a schema per table, checked against the record size); changing records; a file overlay so mods ship replacement data files; mod text; mods loading before the game's data |
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
| `gear/lua_host.*` | The LuaJIT state on the engine thread, native functions for the kernel, time budget |
| `gear/lua/kernel.lua` | The trusted Lua side (embedded as a resource): manifests, load order, sandboxes, events, table views |
| `gear/tables.*` | The engine's data tables, read in place with bounds and fault checks |
| `gear/game/gear_addr.h`, `gear_sigs.inc` | The sacred.exe addresses Sacred Gear uses; their signatures (generated) |
| `gear/examples/` | Example mods |

Tools (Python 3 with `pefile` and `capstone`, `SACRED_ENG` as for SacredBild's tools):

| Tool | Does |
|---|---|
| `tools/gen_gear_sigs.py` | Writes `gear/game/gear_sigs.inc` from the entries listed in it |
| `tools/check_gear_overlap.py` | Fails if a Sacred Gear signature window overlaps one of SacredBild's |

Build: part of the normal build (`cmake --build --preset release` -> `out/build/msvc-x86/RelWithDebInfo/SacredGear.dll`);
`-DSACREDGEAR=OFF` leaves it out. vcpkg provides LuaJIT, statically linked like everything else.
