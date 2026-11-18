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
    author = 'You',
    description = 'Says hello in the log.',
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
| `name`, `version` | Shown in the Mods screen and the log; `version` is part of the mod set |
| `description`, `author` | Shown in the Mods screen |
| `priority` | Lower loads first (default 0); ties load by id |
| `requires` | Ids that must load first; the mod is skipped if one is missing (and off while one of them is off) |
| `enabled` | `false`: off until the player switches it on in the Mods screen (a default, not a lock) |
| `settings`, `keys` | Declared settings and key binds: the [Mod manager](#mod-manager) builds their pages |
| `config` | Default settings: `gear.config` starts as a copy, and `config.lua` in the mod's folder (a table, values only) overrides them. Players edit `config.lua`. A declared setting with the same id takes its default from there. |
| `pipes` | Names of the named pipes the mod may open (`gear.pipe.open`) |
| `automation` | `true`: the mod gets `gear.auto` when the game runs with `-gear-auto` |

### Lua API

The complete reference, with every parameter, option and return value, is [GearApi.md](GearApi.md). It is generated from the LuaLS annotations in `gear/meta/`, which give editors completion and checks for mod code (setup: [Editor support](GearApi.md#editor-support-lua-language-server)); `tools/check_gear_api.py` fails when they and the code drift apart.

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
| `gear.settings`, `gear.keys`, `gear.mods` | The mod manager's side: this mod's declared settings, its key binds, the list of mods and a page of its own in the Mods screen. See [Mod manager](#mod-manager). |
| `gear.log(...)`, `print(...)` | A line in `SacredGear.log`, prefixed with the mod's id. |
| `gear.time()` | Seconds, high resolution, from an arbitrary start. |
| `gear.version`, `gear.mod` | Sacred Gear's version; this mod's `id`, `name`, `version`, `dir`. |
| `require(name)` | Runs `name.lua` of this mod's folder once (`a.b` is `a\b.lua`) and returns its result. |

Events: `'load'` (all mods loaded), `'frame'` (`dt`, every in-game frame), `'draw'` (`state`, after the frame handlers: see [UI drawing](#ui-drawing)), `'session.start'` (`hero`, a creature
object: a game with a hero runs), `'session.stop'` (that hero is gone: noticed with the next game's first frame when
the player left to the menu), `'game.saved'` (`slot`, `info`) and `'game.save_failed'` (`slot`, `message`: the outcome of a `gear.game.save`), `'game.saved_other'` (`slot`: a save the game or the player made, see [Saving](#saving)), `'unload'` (the player switched this mod off in the Mods screen), `'settings.changed'` (`id`, `value`: one of this mod's settings changed), `'idle'` (automation only: every 50 ms in the menus), and the custom events mods emit.

#### Data

| | |
|---|---|
| `gear.data.object_types` | The type records every object refers to (0x80 bytes; id = index, 1..0x7E5F): `:name(id)`, `:kind(id)`, `:find(name)` (id, any case), `:search(text, limit)` (`{id, name, kind}`), and the record reader below. |
| `gear.data.creature_types` | `Pak\Creature.pak`'s records (0x56 bytes). |

Both are read only and have `:count()`, `:stride()` and `:get(i)` (record `i` from 0, or nil); a record reads fields by
byte offset: `:u8(off)`, `:i8`, `:u16`, `:i16`, `:u32`, `:i32`, `:f32`, `:str(off, max)`; nil if out of range.

#### Abilities, game text, settings, file overlay

- `gear.data.abilities[id]`: the exe's two ability tables as one table by id (the combat arts, ids from 1000, and the spells, ids from 1).
  A record's fields read and write by name (`effect_base`, `value0_per_level`, `name_text` ..., `unk_<offset>` where no consumer
  proves a name); writes go to the live tables, are checked for type and range, and are undone by `abilities:restore()` and when the
  mod is switched off. `abilities:apply({[id] = {field = value}})` checks everything first and then writes it. `gear.text.get(text_id)`
  gives the game's text for a name or description id; `gear.settings.get(key)` a value of `Settings.cfg`.
- The file overlay serves a mod's own data where the game opens the original files. `mods/<id>/overlay.txt` has one rule per line
  (`#` comments): `binalance.bin = dataalance.bin` (the game's open of that path, relative to the game folder, gets the mod's file
  if it exists) and `saves = save` (the game's `Save\` folder is the mod's `save\` folder; the vanilla saves are neither read nor
  written). The rules are read when Sacred Gear starts, because the game opens its data files before any Lua runs, for the mods the
  run uses (`-gear-mods=a,b`, else all) and that are switched on (a mod that is off gets no rules and no save redirect, so
  it changes nothing: LAN games need the client and the gameserver to read the same `balance.bin`, and the gameserver is not
  overlaid; a mod that overlays `balance.bin` is for singleplayer). `gear.files.overlay()`, `gear.files.resolve(path)` and `gear.files.served(path)` show what applies and
  what the game really opened through it.
  **The gameserver.** A hosted or LAN game runs `GameServer.exe`, which reads `binalance.bin` and the scripts itself and
  compares them with the client's. Sacred Gear hooks the game's `CreateProcessA`: when it starts `GameServer.exe` (and some
  enabled mod has file rules) it starts it with `SacredGear.dll` injected (Detours, as SacredBild does for its own network
  patches; both can be injected at once) and hands it the ids of the mods the client overlays in the environment variable
  `SACREDGEAR_OVERLAY_MODS`. In the server Sacred Gear is headless: it only installs the same file rules of those mods
  (not the saves redirect: the server has no savegames) and logs `Overlay: the gameserver opened ...` the first time it
  gets a mod's file (`SacredGear-server.log`, in the game folder). A mod that is off, or no mod with file rules, means no
  injection at all; a `GameServer.exe` Sacred Gear was not asked to take part in (no handoff variable) is left alone. The
  server sees the very files the client does, so a mod's balance works for LAN and hosted games. Test:
  `tools\server-overlay-test.ps1 -GameDir <copy of the game folder>` starts the server alone; `tools\lan-host-test.ps1 -GameDir <copy> -Patch` hosts a real LAN game through the menus with a changed balance.bin (without the server overlay the game shows "IP: Kann nicht verbinden! CODE -10021"). Two clients joining one server is not covered by these tests.
- A `patch:` rule ships only what differs from the game's own file: a Lua data file `return {size = n, crc32 = 0x..., ops = {{offset, 'hex'[, replaced]}, ...}}`
  (offsets in the original, ascending; the hex bytes replace `replaced` bytes, default as many as there are). The first open of the file builds the
  patched copy in the mod's `cache\` folder (rebuilt when the patch or the original changed) and serves that; a game file of another size or
  crc32 than the patch was made for is served unpatched and logged. The file is read by a plain Lua state without libraries, so it can only be data.
  Two more kinds: `kind = 'words'` (`ops = {{first word index, {value, ...}}, ...}`, a value is an unsigned integer or `{f = float}`; for tables of 32 bit
  words like `balance.bin`) and `kind = 'texts'` (`set = {[text id] = 'text'}`, `remove = {ids}`; for a language's `global.res`).
  Names: every `.lua` file in the mod's `constants/` folder returns a table of names to numbers and is a global of its upper case name in the
  patch files (`constants/creature.lua` -> `CREATURE.WOLF`, `ITEM`, `OPCODE`); the cache is rebuilt when they change.
  `kind = 'words'` has `ops` (`{first word, {values}}`) and, for tables with records, `records = {first = word index of record 0, words = words per
  record, fields = {name = word in the record}}` with `edits = {{record = n | records = {from, to}, field = value, ...}}` (also as `sections = {{records,
  edits}, ...}` next to `ops`).
  And `kind = 'script'` for the compiled script files `FunkCode.bin` / `StartCode.bin` (streams of instructions `u16 opcode, u16 length, operands`;
  FunkCode.bin is cut into named functions by the `Vectoren.bin` next to it, a table of 84 byte records `char name[64], i32 start, i32 length, ...`):
  `file = 'funk' | 'start'`, `edits = {...}` and no `size`/`crc32`. Edits find their instruction themselves:
  `{fn = 'Sector88067Enter', op = OPCODE.FILL_SECTOR, sector = {88, 67}, [set_sector = {x, y},] spawn = {old = {{creature = CREATURE.X, weight = 10, group = 1}, ...}, new = {...}}}`
  (the `FillSector` instruction of that function for that sector: the spawn table that ends it, 13 bytes per entry after a 40 byte header, and/or the
  sector it fills; `ins = n` only if the sector alone is not unique), `{op = OPCODE.SET_VAR, var = 'Name', old = n, new = n}` (the `SetVar` instruction of
  that variable, StartCode.bin) and `{fn, op, ins = n, at, old = 'hex', new = 'hex'}` (a byte range in the n-th instruction). The function and the instruction
  must be unique, the opcode must match and `old` must be what the file holds, or the whole file is served unpatched and the reason is logged. One patch file
  can serve many game files (all hero types share the same library code). Format notes in sacred-gear-reborn-decomp's `docs/file-formats.md`.

#### World

Positions are world units (`gear.world.TILE`, about 53.7, is one tile; the screen shows about 534 x 400 around the
hero) and a layer (0 outdoors; dungeons have their own). The functions that change the world work only in single
player (`gear.world.can_change()`); in a network game (also a host nobody has joined yet: the engine tells the peers about every created or destroyed object, and they do not know the ones a mod makes) they return nil and a message. `can_change()` reads the engine's own network flag (the byte at 0x0182EBEC that its object and inventory code test), not the number of heroes.

| | |
|---|---|
| `gear.world.hero()` | The local hero (a creature object), or nil. |
| `gear.world.object(id)` | The object with `id`, or nil. |
| `gear.world.creatures_near(x, y, radius, {layer, limit})` | Creatures there, nearest first (default limit 64). |
| `gear.world.around(x, y, min, max)` | A random point between `min` and `max` from (x, y). |
| `gear.world.spawn(type, x, y, {layer, label})` | Creates an object of `type` (a type id or name) there, as the game's own scripts do; the creature, or nil and a message. `label` is kept for the creature this session (`c:label()`; shown in game in a later version). |

A creature object `c` (`c.id`) reads the game each time; after the object is gone the reads give nil:
`c:exists()`, `c:info()` (all fields), `c:type()`, `c:type_name()`, `c:position()` (x, y, layer), `c:sector()` (the sector table slot of the position), `c:hp()`,
`c:max_hp()`, `c:is_dead()`, `c:level()`, `c:experience()`, `c:gold()`, `c:distance(other | x, y)`, `c:label()`;
changes: `c:set_hp(hp)` (1..max: mods do not kill through it), `c:set_gold(n)`, `c:add_gold(n)` (the local hero only, also in network games: through the engine's own gold setter, which keeps its anti-cheat copy in step and sends the change with the hero's next update),
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
at the items around the hero every fraction of a second and hands over the current list (the `lootfilter` mod in [sacred-gear-mods](https://github.com/kirides/sacred-gear-mods)).

#### Saving

| | |
|---|---|
| `gear.game.can_save()` | `true`, or `false` and why not: a living hero in a single player game, the game shown (no cinematic, window drawn alone, escape menu or message box), no save or load pending. |
| `gear.game.save(slot, name, {thumbnail})` | Asks the engine to save into `Save\GAME<slot>.PAK` (slot 0 the quicksave, up to 509), as the savegame window does; `true` when accepted, else nil and a message (the engine refuses while a trade, chest or similar window is open). `name` is the display name (UTF-8, 60 characters; kept in Windows-1252 like the game's own names, other characters become `?`). The result comes as the events `'game.saved'` / `'game.save_failed'`. |
| `gear.game.save_pending()` | Whether a save or a load is queued or being written. |
| `gear.clipboard.set(text)`, `get()` | The Windows clipboard (Unicode text, at most 1 MB): `set` returns `true`, or nil and why; `get` the text on it or nil. For a mod that shows something the player wants to paste elsewhere. |
| `gear.game.config(key)`, `gear.game.config_all()` | The game's own settings from `Settings.cfg`, read only (numbers as numbers, the key in any case; nil if absent): what the game was set to, not a live view; the options window writes the file on OK and many settings are read at start only. For warnings about clashes, e.g. `gear.game.config('AUTOSAVE') == 1`. |
| `gear.game.save_info(slot)` | `{exists, size, mtime}` of the slot's file (`mtime` in unix seconds), so a mod can rotate slots oldest first. |
| `gear.game.saves()` | Every savegame (`Save\GAME*.PAK`, or a mod's save folder), sorted by file name like the game's list: `{slot, file, name, hero, class, class_id, level, version, compatible, time, mtime, size, world_date, picture}`, read from each file's header and chunks (hero name, class and level, the time it was saved). |
| `gear.game.save_picture(slot)` | The savegame's picture (128 x 96, ARGB 4444 in the file) as an id for `gear.ui.image`. |
| `gear.game.load(slot)`, `gear.game.delete_save(slot)` | Loads a savegame (single player, in game or from the menus) as the savegame window's Load does; deletes its file (the game's savegame windows read their lists again). |
| `gear.game.save(slot, name, {window = true})` | Saves as the open savegame window's Save button does (for a replacement of that window): the picture the game took when the window opened, and the window closes. |

The game's own **Auto-Save** option (`AUTOSAVE : 1` in `Settings.cfg`, the options window's "Auto-Save") saves when a script runs command number 0x79 (quest and map scripts place it) into the *first free* slot `GAME01`, `GAME02` ... (every one a new file, never overwritten), in single player only; Gear reports it, the quicksave and the savegame window's saves as `game.saved_other`.

The engine writes the file a moment later on its render thread; Sacred Gear sees the write end (the engine's flags)
and compares the file before and after. See `autosave` in the sacred-gear-mods repository for a periodic autosave.

#### Inventories

| | |
|---|---|
| `gear.inv.container(which)` | A container (an inventory grid): `'chest'`, `'backpack'` (the local hero's) or its number (0..31; the backpack of the hero with object id n is n, the chest is 17), or nil. `.index`, `.columns`, `.rows`. |
| `c:items()` | The items in it in cell order: `gear.world.item` tables plus `col`, `row` (top-left cell, from 0), `w`, `h` (cells). |
| `c:free_spot(w, h)`, `c:put(id, col, row)`, `c:take(id)` | Room for a w x h item; puts an item at a cell; takes one out (the object stays). |
| `gear.inv.export(id)` | The item's type and own data (its random properties) as a binary string. `gear.inv.hex(bytes)` and `gear.inv.unhex(text)` make it text for JSON. |
| `gear.inv.create(bytes)`, `gear.inv.add(c, bytes)` | A new item from those bytes, at the hero's feet, or straight into the container's first free spot (nothing changes if it fails). |
| `gear.inv.socket(id, upgrade_id)` | Puts an upgrade item (a rune, made with `create` or lying in the world) into a free socket of an item, as the blacksmith does (the item gains its bonuses and an entry, the rune is consumed). |
| `gear.inv.remove(c, id)`, `gear.inv.destroy(id)` | Takes an item out of the container, destroys it and returns its export; deletes an item that is in no container. |

All of it goes through the engine's own inventory and object code, under the fault guard. In single player it works as
`gear.world.can_change()` says; in a network game (LAN, open net; `gear.inv.can_change()` returns `true, true`) only the
local hero's own backpack and the chest may be changed, and Gear then takes the engine's announcing variants: an item
made is announced as created (message 0x16F, `create` with broadcast) and then updated with its data (0x16C, the
engine's `0x005C0810`), put into and taken out of a container with the announcing flag (0x13F / 0x13E) and destroyed
with `0x005FBDB0 (id, 1, 0, 0)` (0x167), the sequence the engine's own loot generation and inventory use, so the other
players and the game server see the same objects. `world.spawn` stays single player only. The temporary items of item icons and tooltips for stored items are local-only
in a network game (see Item icons). Not tested against a second PC; closed online servers that check items are out of scope. An exported item made again is byte for byte the same (type, entries, data):
the self test of `shared-stash` and the probe that found the format moved 55 backpack items through the chest and
back with equal exports. An export holds everything that makes an item what it is: its type, its entries (the random properties, and one entry
for every rune socketed into it: a socketed item is 6 + 130 * entries + 130 bytes) and its record (the bonuses, which a
socketing has already added), and for weapons 16 more bytes (four dwords the weapon class keeps, copied as the engine's
own save does). Blobs exported before weapons kept those 16 bytes still create fine; the new weapon keeps its default
values. Checked in game with armour holding one and two runes and a unique sword with a rune: exports equal after a
deposit and a withdrawal in another savegame and process. What is not copied is where an item was: the engine owns that. Gear exposes no
inventory events yet, so a mod looks at the containers when it needs them (the stash panel refreshes 4 times a second).

`gear.ui.item_tooltip_text(item)` returns its words instead (UTF-8 with the game's colour codes).
An export's tooltip needs no registered object: the engine's object factory (`0x00451300`, stdcall (kind, type, id), kind = the
type record's +0x2E) constructs an item without registering it (no id, no object manager, no sector list, nothing sent to the
network), Gear fills it with the export's entries, record, weapon extras and name id, asks its virtual function 17 for the text
and deletes it with its own destructor (vtable slot 0, flag 1). Checked on all 55 items of a backpack: the words are identical
to those of the live items, sockets and unique names included. The export carries the item's own name id (object +0x3C) since
then: before, a rare or unique item came back from the stash with its type's plain name.

`gear.ui.item_tooltip(item, x, y)` shows the game's own tooltip popup for an item (the display name in its rarity
colour, price, protection or damage, level, bonus lines, socket count, in the game's framed style): an item table, an
item id, or an `export` string (a temporary item is made to read the text from). `x`, `y` are the popup's top-left
corner (it keeps itself on the screen); call it each frame it should show. It returns the popup's size as of its last
layout, so a mod can put the next one beside a region. See the Engine notes ("Item tooltips").

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
| `item_icon(item_or_type, x, y, w, h, opts)` | What the inventory shows for an item: its own 3D model, drawn by the game's model renderer as large as fits the box (x, y, w, h), centered in it, with the same `layer` / `window` options as the other draws. `item_or_type`: an item table (from `gear.world.item` or a container's `items()`: the live object is drawn with its own state and rarity tint), or a type id or type name (what a stored item such as the shared stash's has left): then an item of that type is made for the frame and destroyed after it. See [Item icons](#item-icons). |
| `images(filter)` | The UI images whose sheet name contains `filter` (any case): `{id, sheet, width, height, loaded}`. Ids are the game's image indices (stable for a build). |
| `image_info(id)` | One of those, or nil. |
| `fonts()` | The number of the game's UI fonts. |
| `text_size(text, font)` | Width and height of the text in the layout, or nil. `font`: a game font (`'game1'`, a number), or a table of `text`'s font options (`{size = 16, bold = true}`); nil: the default UI font. Game fonts in game only (measuring may add glyphs to the font). |
| `window(name)`, `windows()` | The game's in-game windows by control name (`UI_WND_INVENTORY`, `UI_WND_TASKBAR`, ...; see `docs/UI_SYSTEM.md`): `{name, slot, x, y, width, height, visible, drawn}` in the window's own layout coordinates; `drawn`: open and drawn this frame (not hidden behind a window the game draws alone). |
| `state()` | `{available, mode, exclusive, escape_menu, message_box, help, show_text, mouse_x, mouse_y, screen_left, screen_top, screen_right, screen_bottom}`: `show_text` whether the game shows its item and object labels (Alt held; the flag its label pass sets, ENG 00627180); `mode` is `'game'`, `'menus'`, `'cinematic'` or `'none'`; `exclusive` the control name of the window the game draws alone (options, save, character, world map), if any; the cursor in layout coordinates; the screen's edges in layout coordinates (beyond 0..1024 x 0..768 when SacredBild's canvas leaves room around it). |
| `screen()` | Those four edges. |
| `tooltip_rect(w, h, opts)` | Where to put a `w` x `h` tooltip (or any popup beside the cursor): below and right of the cursor's tip, clear of the game's cursor sprite (about 30 x 38 layout units); flips to the left / above at the screen's edges (or `opts.bounds = {left, top, right, bottom}`), clamped as a last resort. Returns `x, y`. `opts.x`, `opts.y` give another point to keep clear, `opts.cursor = false` keeps only a small gap. The widgets' `tooltip` option uses it. |
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

#### Item icons

`gear.ui.item_icon` draws an item the way the inventory windows do: the game's `cInventoryEntry_render` (0x5DC430) hands the
item's Granny model to the model renderer (singleton 0x401660, draw 0x40E9C0: `device, model0, model1, x, y, type, scale,
0, flags, 0, 0`, centered at (x, y) of the 1024x768 layout, scale 1 for the inventory's 32 units a cell). Gear calls the same
code inside its UI pass with the game's states, so it works with the vanilla ddraw and with SacredBild's backend, layers
and `window` anchoring included; the device states, transforms, viewport and the render flags are read before and put
back after.

- **A live item** (`item.id` is an item object, in the world or in a container): its own model state is used (made on
  first use, as the inventory does) and its flags (`+0xB0 & 0xF00000`: the rarity tint). Cheap.
- **A type id or name** (a stored item): no item object is made at all. The icon takes the type's model pair from the
  model loader (`0x004115D0` singleton, `0x00412590 (loader, name, out[2], description)` with the type's model name and
  description from `0x00426250` / `0x004262B0`), the same call the item's own model state makes, and hands it to the model
  renderer; the pairs are cached per type while icons ask for them and given back (`0x00403E10`) a few seconds after the last
  request, at most 160 types. No object manager, no id, no network, in every game kind; there is no rarity tint (that comes
  from the live item's flags). A type the loader cannot load falls back to a temporary item: made at the hero's feet, at once put into
  container 31 (never in the world), drawn, taken out and destroyed after the layer's commands; in a network game made
  local-only (no broadcast, no announcing flag, destroyed through the engine's local path `0x005FBDB0 (id, 0, 0, 0)`), at
  most 48 types a frame, a few tenths of a millisecond per type and frame: draw a stash list's visible
  part only, and prefer live items where there are some.
- Nothing is drawn when the type is no item or has no model, without a hero, or when the
  addresses were not found (the log says so at start).

Self test: `gear/examples/itemicontest` draws a row by type id and a row from live items.

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
| `dropdown(id, x, y, w, options, index, opts)` | `changed, index`. The open list is drawn after every mod's draw handlers, over all panels and text on the top layer, and takes the mouse first (nothing under it reacts); it shows at most `opts.max_rows` (12) entries and scrolls like a scroll area, the selected one in view; a click beside it closes it. |
| `slider(id, x, y, w, min, max, value, opts)` | `changed, value` while dragged; `opts.step` (default 1), `opts.format` (`'%g'`; false: no number). |
| `label(id, text, x, y, opts)` | Text in the widgets' look (the UI font; `size`, `font`, `height` / `valign` as for `text`), with a tooltip if `opts.tooltip`. |
| `panel(id, title, x, y, w, h, opts)` | A window: `{open, x, y, w, h, content_x, content_y}`. Its title bar drags it (the position is kept for the session), its X closes it (`open` false); `show_panel(id, open)` and `panel_open(id)`. Whether a panel is open is kept in `mods\gear_layout.json` and comes back at the next start (what `main.lua` sets with `show_panel` is only the first-start default); `persist_panel(id, false)` opts a panel out. `opts.closable`, `opts.draggable` (default true). `opts.bare`: no window at all (no background, title bar, X or grip, no region: the mouse goes on to the game); the mod draws in the whole rectangle, the layout editor still moves it, for HUD elements. |
| `scroll_begin(id, x, y, w, h, opts)`, `scroll_end(content_height)` | A vertically scrollable area. `scroll_begin` returns `{x, y, w, h, offset, max, content_x, content_y}` (the viewport, less the scrollbar when it shows); draw the content at `content_y` plus its own y, then `scroll_end(content_height)` (returns `offset, max`) draws the scrollbar: arrows (held: repeat), the track (a click pages), the thumb (drag it). Until `scroll_end` everything drawn is clipped to the viewport (rects, images and the UI font's text are cut; the game's fonts and item models show only when wholly inside) and regions are cut to it, so a widget scrolled out of sight takes no click. The wheel scrolls the area under the cursor (3 lines of `opts.step`, default 20, a notch); Up, Down, PageUp, PageDown, Home, End the one under the cursor or clicked last; with SacredBild the controller's D-pad the one its cursor is in (once no widget lies further that way inside it) and the right stick as a wheel. The offset is kept per id for the session; `scroll_to(id, offset)`, `scroll_offset(id)` -> `offset, max`. Areas nest. |
| `wheel()` | The wheel's notches over mod UI this tick (+ away from the player), and whether a scroll area took them. |
| `text_wrap(text, width, opts)` | `lines, height`: the text broken into lines that fit `width` in `text`'s font options (at spaces, `
` too), and their height; cached. |
| `region(id, x, y, w, h, opts)` | A rectangle that takes the mouse, for widgets of a mod's own: `{hover, pressed, released, clicked, held}`. |
| `mouse()` | The cursor in layout coordinates. |
| `capture_keys(on)` | While on, the keyboard does not reach the game (its key and character messages are dropped, its key state queries see nothing held); the mod's own `key_pressed` / `key_down` still work. For a screen that types or binds keys (the mod manager uses it while it is open); ends when the mod unloads. |
| `typed()` | What the player typed since the last tick while this mod captures the keys (`capture_keys`): UTF-8 in the player's keyboard layout (umlauts, Shift, AltGr), no control characters (Backspace and Enter with `key_pressed`). For a text field. |
| `key_pressed(name)` | True once per press of a key (`'f9'`, `'a'`, `'escape'` ...), read while the game's window has the focus. |
| `key_down(name)` | Whether the key is held. Both see the keys [automation](#automation) holds; isolated automation sees only those. |
| `COLORS` | The widgets' colours. |

The mouse wheel over a mod's region is the mod's: the game does not get it (no zoom under a panel). It comes through SacredBild's window procedure (the player's wheel and the controller's right stick), or without SacredBild through a message hook Gear sets on the game window's thread.

Input: a left click (press and release) over a mod's region is taken from the game: the UI manager's event handler
(0x00757840) reports it handled, so neither the game's windows nor the world see it, and the release that follows
goes to the same region. Regions on the world and hud layers take nothing while the escape menu or a message box is
up. Hover is decided against the previous frame's regions (the topmost under the cursor), so an open dropdown hides
what lies below it. Keyboard input is `key_pressed` / `key_down`, and `typed()` for text while the mod captures the keys. A press or release also puts
`mouse()` at its own position for that tick.

While the cursor is over a mod's region (any widget, panel or the layout editor's overlays), the game's windows do
not see it: with SacredBild, their reads of the cursor give a point far off the screen, so they neither hover nor show
item tooltips under mod UI (the held item still follows the real cursor), and the world picks nothing there.

#### Replaced windows

A mod can draw its own screen in place of one of the game's windows: `gear.ui.replace_window(name)` claims it
(`UI_WND_SAVEGAME` and the other in-game control names; the menus' screens `UI_MENU_MAIN`, `UI_MENU_CHARACTER`,
`UI_MENU_OPTIONS`, `UI_MENU_LOADGAME`, `UI_MENU_NETWORK`). The game's window still opens and closes as it always does
(hotkeys, the escape menu, its own commands), so the game's state is the one it expects; while it is replaced, its
`render` and `receiveEvent` (vtable slots 5 and 4 of its class, patched only once a mod claims the window, after
SacredBild wrapped them) draw nothing and swallow mouse and key events (named UI events still reach it, so it can be
shown and hidden). Each tick the mod draws its screen it calls `gear.ui.replaced(name)` (true while the window is open);
without that call for 0.5 s (1.5 s after the window opened), or when the mod is off, unloaded or switched off after
errors, the game's own window shows again until it closes. `gear.ui.window_open(name)` and `gear.ui.close_window(name)`
(as the window's Back / Esc) go with it. A replacement usually draws on the `top` layer and takes the keys
(`capture_keys`) and the controller (`nav_modal`) while it is up. See `better-saveload` in the sacred-gear-mods
repository.

#### Layout: persistent panels, movable game windows

Every panel has an id, `mod_id:panel_id` (`panel().key`). Where the player drags it (and how large they make it, with
`opts.resizable`, `opts.min_width`, `opts.min_height`) is kept in `mods\gear_layout.json` and restored at the next
start. Places are stored in a virtual 4096 x 4096 screen (the whole screen, whatever its resolution and aspect), so a
layout keeps its places when the resolution changes. The game's own windows are kept there too, as
`game:UI_WND_INVENTORY` and so on, and placed every frame.

| | |
|---|---|
| `edit_mode()`, `set_edit_mode(on)` | The edit mode: a flag for every mod (panels get a highlighted frame); a layout editor turns it on and draws its overlays (the `layout` mod in [sacred-gear-mods](https://github.com/kirides/sacred-gear-mods), its "Edit layout" button while Alt is held). |
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
folder, `*.json`, `*.txt` or `*.lua` other than `mod.lua` and `main.lua`, at most 8 MB, written atomically): for settings a mod's own UI
changes. A write is atomic (a temporary file moved over the old one), so a crash leaves the old file or the new one.

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

### Drops

`gear.drops` tunes what creatures drop without touching the game's code unless a mod asks: `chance_scale` (how often), `rarity_weights` (per quality
multipliers of the rarity table, also for set and unique drops), `find_scale` (how much the rarity bonus matters), `rng` (the game's own random numbers, a
fair generator without the CRT `rand`'s modulo bias, a seeded one, or a Lua function) and the events `drops.roll` (veto a roll, change its rarity bonus) and
`drops.select` (change or replace the item the loot code picks). The loot code runs in `sacred.exe` on the machine that owns the creature (GameServer.exe has none).
What the engine does, what each call touches, the limits and the tests: [Gear drops](GearDrops.md); the calls: [reference](GearApi.md#drops-geardrops).

### Party

`gear.party` gives the player companions in single player: more heroes of the player at the hero ids 3 to 16, the way a
network game holds the players' heroes. Each has its own backpack, equipment, attributes, skills, combat arts, spells
and experience, and is saved with the savegame like everything else in the world. `recruit(class, {experience = n})`
creates one where the leader stands (as the engine creates the player's hero: level 1, no items; the experience goes
through the game's own gain, so it levels up with skill points). `control(hero)` gives the player another hero: the
camera, the HUD, the windows, the mouse, the keys and the controller serve it, `gear.world.hero()` is it, the gold goes
with it (the party's purse) and the mercenaries follow it; every mod gets `party.control` `(from, to)`. The heroes the
player does not control stand still until a mod orders them: `creature:order_move(x, y)`, `order_attack(target)`,
`order_ability(id, target | x, y)` (a combat art or spell it knows, refused while it regenerates: `creature:abilities()`
lists them with `regen`), `order_stop()`; `creature:action()` says what one does, `creature:is_enemy(other)` what the
cursor's attack symbol would say. `share_experience(share, radius)` makes the experience a party hero gains from the
game go to the others too. Loading a savegame gives control back to the hero with id 1. Id 2 is never used: the engine
moves object 2 with the arrow keys in single player (a developer feature).

In a network game the functions refuse: there the hero ids belong to the players, and objects tick only on the
computer that owns them (see the engine notes). What it touches: [reference](GearApi.md#party-gearparty).

### Mod manager

Sacred Gear has a **Mods** screen, a built-in part of it (`gear/lua/modmanager.lua`, shipped inside `SacredGear.dll` and
loaded by the kernel as a mod of its own, `gear-mods`). It lists every installed mod, a page of six at a time, with
its name, version, author, a short description, an On/Off switch and the mod's state: switched off after ten errors,
off because a mod it requires is off, a key shared with another mod. Each mod has a **Settings** page (made from the
settings it declares, plus a page of its own if it registered one) and a **Keys** page. The screen takes the mouse
while it is open, like the other mod panels, and so does the keyboard: no key reaches the game (see `gear.ui.capture_keys`),
so typing into a text setting does not fire its hotkeys.

**Players.** Press **F11** in game, or click the **Mods** button under the game's escape menu (in the main menu the
button is at the bottom left, but only a run with `-gear-auto` ticks there). **Esc** closes the screen (the game does not
see that key, so its escape menu stays as it was); so does the Close button, the X, or F11 again.
Every choice is saved at once in `mods\gear_mods.json`: the On/Off flags, the settings and the key binds of all mods in one
file, written atomically (a temporary file moved over the old one); if the file cannot be read it is kept as
`gear_mods.json.bak` and the defaults apply. A mod switched off in the screen is unloaded at once (it gets `unload`;
its handlers, timers, tasks, pipes, key binds, pickup rules and open widgets are dropped; what it did to the world
stays) and is not loaded at the next start. Switching it on loads it again, fresh, as if the game had started with it
(`load`, and `session.start` when a game runs). A mod switched on brings the mods it requires with it; mods that
require a switched-off mod are off while it is, and come back when it does. A mod that raised ten errors shows as
failed: switch it off and on to try again. `enabled = false` in a mod's `mod.lua` only means "off until the player
switches it on". To reset one mod's settings use the **Reset to defaults** button of its page (or **Reset** beside one
setting); **Default** beside a key restores its default. A key that two mods use shows a conflict on both mods' rows
and in the Keys page (both still fire; rebind one).

Default hotkeys of the mods in this repository and the examples: F11 the Mods screen, F4 the widget gallery of `uidemo`; in
`sacred-gear-mods`: F2 / F3 the session tracker and loot log, F5 the loot filter, F6 the autosave, F10 the shared stash and Ctrl with
D, G, J, K, U, Y for the damage meter and the server mods (F7, F8 and F9 are left to the game). The full table:
[Hotkeys](https://github.com/kirides/sacred-gear-mods/blob/main/docs/Hotkeys.md#hotkeys-and-the-clashes-in-the-defaults).
None is a key of the game: the screen marks a bind that is also one of the game's own hotkeys (digits, Q W E R B, A, I, F,
C, L, M, Tab, O, S, H, N, P, Space, Esc, + and -, F8 quick load, F9 quick save; the game reacts to them with any modifiers held) as
a conflict with "The game", like a clash between two mods.

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

`gear.settings.get(id)` returns the player's value if there is one, else the value of `config`/`config.lua` under the
same id, else the declared `default`; a value that no longer fits (a choice that was removed, a number out of range) falls
back one by one. `set(id, value)` coerces the value like the screen does and answers `nil, message` if it does not fit;
the `settings.changed` handlers of the mod run when the value changed. `is_set(id)`, `all()`, `reset(id)` and `migrate`
are in the [reference](GearApi.md#gearsettings). `migrate(name, fn)` reads an old settings file of the mod's folder once
(JSON) and hands the table to `fn`, which copies what it wants into the settings: the migration is remembered in
`gear_mods.json`, so a leftover file is not read twice (the loot filter's old `settings.json` works this way).
`restart = true` marks a setting the mod only reads at start (the screen says "after restart"; the mod still gets
`settings.changed`). Keep `gear.config` for what has no screen (lists, positions, self test switches).

A declared key is bound to its `default` until the player changes it; `''` means unbound. `gear.keys.binding(id)` tells the
current binding. Two mods with the same binding both fire; pick a default that is free (the table above) and the player
can rebind. Hotkeys of other mods stay quiet while the Mods screen is open. The text of a binding is lower case,
modifiers first: `ctrl+shift+m`, `alt+f5`; keys are named as in `gear.ui.key_pressed`.

**Remapping the game's own keys (`gear.input`).** The game's keys are fixed (I inventory, 6 to 0 combat arts, Space the
healing potion ...). `gear.input.remap(rules)` lets a mod turn another key or mouse button into one of them:
`{from = 'mouse4', to = 'i'}` makes the first side button open the inventory, `{from = 'i', block = true}` makes the
original I do nothing. Sacred Gear changes the messages the game reads (before its handlers: nothing sees the original
press), swallows the character the old key made and gives the game the new key's own, however Shift or Ctrl are held.
It applies only while the game has the keyboard (in game; not with a message box, the escape menu, the savegame, options
or character screen or the chat line open), so typing and menus are never touched, and all keys are the game's own when no
mod sets rules. A rule with an `id` also reports presses to `gear.input.on(fn)` (a quick cast mod uses that to add a
click). Inputs are the key names of `gear.ui.key_pressed` (now with `f13` to `f24`) and `mouse3` (middle), `mouse4`,
`mouse5`, with modifiers (`shift+mouse4`). Declared keys in `mod.lua` take the same names, so the Mods screen rebinds
them with its key capture and the controller's key picker. `gear.input.capture_start()` / `capture()` give a screen of
a mod's own the same capture. A key the mod took over (a rule for it, even `{from = '6', to = '6'}`) is no longer listed
as a clash with the game's hotkey in the Mods screen. See the [Key bindings mod](https://github.com/kirides/sacred-gear-mods/blob/main/docs/mods/key-bindings.md).

`gear.mods.list()` and `gear.mods.get(id)` describe the installed mods (state, enabled, errors ...) for a mod that
wants to know about another one, e.g. to offer a feature only when a companion mod runs. Switching mods is the
player's choice; mods cannot do it.

What does not unload: a mod that hooks the game itself would have to; Gear's own hooks (pickup rules, drawing) are
dropped with the mod. If a mod keeps state in the engine (spawned creatures, items), that stays until the game
restarts. Settings that need a restart should say so (`restart = true`). An unload does not run in the middle of a
frame: switching a mod on or off takes effect at the start of the next tick.

Self test of the screen: `gear/examples/modmanagertest` (see its header) clicks through the real screen in a game and
checks the results; `gear/tests/kernel_test.lua` covers the kernel side (persistence, broken files, switching mods live,
dependencies, key binds and conflicts) and drives the screen against a mocked UI.

### Realms

`gear.mp.mode()` tells the kind of game: `'single'`, `'lan'`, `'open'` or `'closed'` (Closed Internet: the heroes belong to lobby
accounts and the server keeps them). Default-deny: it reads the engine's net object (+0x3818 the menu's Internet entries were
used, +0x3819 the announcement's "lobby identity checked" bit) and the hero's source (the dispatcher's case for message 0x132, the
character block the game server delivers in Closed Net, is watched). An Internet game is `'closed'` unless Open Net is
proven: the receive hook is in, the hero has been in the game for 10 s and no 0x132 came. `'lan'` only through the LAN menu.
Nothing a mod or the player sets decides it; whatever cannot be read counts as `'closed'`. `gear.auto.network_mode(mode)` and
`gear.auto.hero_from_server(on)` set the flags for self tests.

`gear.realm` chooses the server of the game's online play (register and login), so that several servers (a private
realm, for example) can be used without editing `Settings.cfg`. The game reads the lobby host and port when the
multiplayer menu opens **Closed Internet** or **Open Internet**; Sacred Gear hooks those steps (`gear/realm.*`) and
puts the chosen realm's host (and its port, if it names one) in place of `NETWORK_LOBBY` / `NETWORK_LOBBYPORT`. LAN is
not touched, and `Settings.cfg` keeps the game's own values (the hook on the object's write-back swaps them for the
moment), so choosing the default realm is the game's setting again. The choice is saved in `mods\gear_realm.txt` (read
by the native side at start, before any mod loads); the list of realms and the active id are in `mods\gear_realms.json`
(`{"realms": [{"id", "name", "host", "port"}], "active": ...}`, also written by `gear.realm.add`). A realm applies from
the next opening of Closed or Open Internet, not to a lobby connection that is already open. The `realms` example mod
shows the choice as a page of the mod manager (F11 in game, or the pad's mod menu); Sacred Gear itself runs only once a
game has started, so the choice is made in game or by editing the files, not in the menus. A realm decides where the
player's password goes: use only realms you trust.

### Errors

An error in a mod's code is logged with its traceback and does not reach the game. A mod whose handlers raised ten
errors is switched off until the game restarts. A handler that runs too long (an endless loop: 0.25 s for a frame's
handler, 5 s for loading) is stopped with an error; LuaJIT checks this only in interpreted code, so a loop the JIT
compiled may not be caught. The game's own functions that Gear calls for a mod run under a fault guard: a call that
faults returns nil and a message instead of crashing the game.

### Examples

User documentation of the session mods and of the client mods of the game server's Lua mods (install, hotkeys, settings, panels): [the docs of sacred-gear-mods](https://github.com/kirides/sacred-gear-mods/tree/main/docs).

`gear/examples` in the repository (the mods players use, such as the loot filter, the layout editor, the realm picker, the Twitch Integration and the server mod clients, live in [sacred-gear-mods](https://github.com/kirides/sacred-gear-mods); copy them from there or from its release zip):

| Mod | |
|---|---|
| `hello` | The smallest mod: events and the creature types. |
| `uidemo` | `gear.ui`: a translucent hero panel, a widget gallery in a draggable, closable panel (F4) with every widget, images in the three fit modes and the fonts, tooltips; a note in the inventory's frame, the world and top layers; with `-gear-auto` a self test that clicks every widget and takes screenshots. |
| `itemicontest` | `gear.ui.item_icon`: a row of weapon and armour models by type id and a row from live items; checks that nothing is left behind. |
| `modmanagertest`, `mmtest_aux`, `mmtest_dep` | The Mods screen's in-game self test, with a helper mod and a mod that requires it. |
| `partytest` | An automation run of `gear.party`: recruits a companion with experience, gives it orders, switches control both ways (the old leader walks on orders, the world keeps ticking), kills an enemy with the former leader's combat art and checks the shared experience, saves and loads with the companion in control, dismisses it (`-gear-load=N -gear-mods=partytest`, a savegame with an enemy nearby; writes savegame 63: use a copy of the game folder). |
| `mptest` | An automation run: the hero's name, key and host, gold through the engine's setter, and a 5000-byte table through the mod channel (`-gear-load=1 -gear-mods=mptest`: the loopback; config `mode = 'lan'`: hosts a LAN game and also sends to its own slot through the real chat carrier). |
| `scrolltest` | An automation run: a scroll area of 100 lines (wheel, a line scrolled out of sight takes no click, thumb drag) and a dropdown of 30 entries whose open list lies over text and a button drawn after it (`-gear-mods=scrolltest`). |
| `tooltiptest` | An automation run: tooltips of buttons at the screen's edges and corners, with screenshots (`gear.ui.tooltip_rect`). |
| `droptest` | An automation run of the drop hooks (`gear.drops`): the engine's loot function is run a few hundred times per phase with vanilla settings, a doubled and a zero chance, rarity weights, a veto, a replaced item, the fair, a seeded and a Lua generator; 31 checks. Use a copy of the game folder: the items stay in the world. See [Gear drops](GearDrops.md). |
| `selftest` | An automation run: loads a savegame, checks the hero, gold, nearby creatures, types, optionally a spawn, writes `GearAutomation\selftest.json` and `types.txt`, screenshots, quits with 0 or 1. |
| `shared-stash` (in the sacred-gear-mods repository) | A stash shared by all heroes of the player, in a panel beside the chest window, built on `gear.inv`: items are copied to `stash.json` in the mod's folder and back, the file written first on a deposit and last on a withdrawal. |

## Automation

For self tests and scripted runs without a player. Off unless the game starts with `-gear-auto` (or
`SACREDGEAR_AUTO=1` in its environment). Then:

| Option | |
|---|---|
| `-gear-load=N` (`SACREDGEAR_LOAD`) | Loads savegame N (`Save\GAMEnn.PAK`, 0 the quicksave) as soon as the main menu is up, the way the menu's savegame window does it. The file is looked up through the file overlay (a mod's save folder counts); if it does not exist the run logs `Automation: cannot load savegame` and ends with exit code 4 (the game itself crashes on a missing savegame). |
| `-gear-mods=a,b` (`SACREDGEAR_MODS`) | Loads only these mods. |
| `-gear-instance=NAME` (`SACREDGEAR_INSTANCE`) | Names the run, default `pid<PID>`. See "Parallel runs". |
| `-gear-window=WxH[+X+Y]` (`SACREDGEAR_WINDOW`) | SacredBild only: the size of the game window, default 1600x900, at least 1024x768 (smaller values are raised to it). See below. |

The mods load in the main menu and get the `idle` event there. Mods with `automation = true` get `gear.auto`:

| | |
|---|---|
| `state()` | `{ui, menus, in_game, cinematic, help, dialog, window, width, height, mouse_x, mouse_y}`: the UI manager's state, the client size, the game's cursor in UI layout coordinates. |
| `ui()` | The visible UI: menus, in-game windows, the message box and their visible children, each `{group, index, parent, vtable, flags, x, y, width, height}` in UI layout coordinates (`parent` indexes the list, 0 top level). |
| `move(x, y)`, `down(button)`, `up(button)`, `key_down(key)`, `key_up(key)`, `release()`, `wheel(notches)` | Raw input; positions in client pixels; buttons `'lbutton'`, `'rbutton'`, `'mbutton'`; keys by name (`'escape'`, `'enter'`, `'f5'`, `'a'`, ...) or virtual key code; `wheel`: + away from the player (scrolls up). |
| In a task: `click(x, y, button)`, `press(key)`, `type(text)` | As a player would, with short pauses. |
| In a task: `ui_click(x, y)` or `ui_click(node)` | Clicks a point in UI layout coordinates (or a node's center); the mapping to pixels is measured once by moving the cursor, so it works with SacredBild's scaled UI too. |
| `load_save(n)` | Loads savegame n from the menus. |
| In a task: `wait_for_menu(timeout)`, `wait_for_game(timeout)` | Until the main menu is up / a hero exists. |
| `instance` | The run's name (a string). |
| `screenshot(name)`, `write(name, text)` | Files in `<game>\GearAutomation\<instance>` (name: letters, digits, `_ - .`). |
| `network_flag(on)` | Sets or clears the engine's network-game flag, to test that the guards refuse in a network game (set it, test, clear it in the same tick). |
| `show_window(name, shown)` | Shows or hides an in-game window by its control name (`UI_WND_CHEST` ...) through the window's own `show`, without the events that normally open it; for self tests. |
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
A normal launch (without `-gear-auto` or `-gear-instance`) has no instance and keeps every vanilla behaviour, the
single-instance check included. What a name changes:

**A second game by hand (manual instance).** `-gear-instance=NAME` without `-gear-auto` makes a *manual instance*, for
testing LAN or online play with two clients on one PC: `sacred.exe -gear-instance=two -gear-window=1600x900` (or
`tools\second-instance.ps1 -Game <folder>`). It gets the single-instance mutex, the window title suffix, and its own log
and `run.json` folder below, and nothing else: no input isolation (it takes the focus and the real mouse like a
normal game), no automation, the player's own `gear_mods.json` and mod switches. With SacredBild `-gear-window=WxH[+X+Y]`
opens a framed window of that size so the two games fit side by side; without it the `.ini` window is used. Start the
first game normally, the second with a name. Both share the game folder, so a savegame and `Settings.cfg` are shared too.

What a name changes:

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
- **`gear_mods.json`** of the mod manager (On/Off flags, settings, key binds) is the run's own copy there, starting empty:
  a test that switches mods or changes settings never touches the player's `mods\gear_mods.json`.
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
| M5: tools | More examples, a reference of every table field; the multiplayer handshake. (The in-game mod list is done: the Mods screen.) |

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
| `gear/realm.*` | Realms: the hooks on the network menu's lobby host and port, and `Settings.cfg`'s write-back |
| `gear/pickup.*` | Pickup rules: the hooks on the game's pickup, the collect filter and the item label colour |
| `gear/itemicon.*` | Item icons: the model renderer call for a live item or a temporary item of a type, with the device state saved and restored |
| `gear/keyblock.*` | The keyboard kept from the game while a screen captures it (`gear.ui.capture_keys`) |
| `gear/ui.*` | Mod drawing: the draw commands, the hooks in the UI pass, Direct3D 7 quads with the game's states saved and restored, the game's fonts and UI images, window and UI state queries |
| `gear/tests/kernel_test.lua` | Tests of the kernel with the native side mocked: `luajit gear/tests/kernel_test.lua` |
| `gear/lua_host.*` | The LuaJIT state on the engine thread, native functions for the kernel, time budget |
| `gear/lua/kernel.lua` | The trusted Lua side (embedded as a resource): manifests, load order, sandboxes, events, table views, the mod catalog (settings, key binds, live load and unload, `gear_mods.json`) |
| `gear/lua/modmanager.lua` | The Mods screen, embedded as a second resource and loaded as the mod `gear-mods`; it is the only mod that gets the kernel's `admin` table |
| `gear/tables.*` | The engine's data tables, read in place with bounds and fault checks |
| `gear/game/gear_addr.h`, `gear_sigs.inc` | The sacred.exe addresses Sacred Gear uses; their signatures (generated) |
| `gear/examples/` | Example mods |

Tools (Python 3 with `pefile` and `capstone`, `SACRED_ENG` as for SacredBild's tools):

| Tool | Does |
|---|---|
| `tools/gen_gear_sigs.py` | Writes `gear/game/gear_sigs.inc` from the entries listed in it |
| `tools/check_gear_overlap.py` | Fails if a Sacred Gear signature window overlaps one SacredBild patches (or one of SacredBild's overlaps one Gear patches) |
| `tools/check_gear_api.py` | Fails if the LuaLS annotations in `gear/meta/*.lua` and the Lua API in `gear/lua/kernel.lua` / `gear/lua_host.cpp` disagree (names, both ways), or if `docs/GearApi.md` is out of date |
| `tools/gen_gear_api_docs.py` | Writes `docs/GearApi.md` from `gear/meta/*.lua` (and `docs/GearApi.head.md`) |

Build: part of the normal build (`cmake --build --preset release` -> `out/build/msvc-x86/RelWithDebInfo/SacredGear.dll`);
`-DSACREDGEAR=OFF` leaves it out. vcpkg provides LuaJIT, statically linked like everything else.

## Engine notes

What Sacred Gear relies on, English GOG addresses:

| | |
|---|---|
| Object manager | `*0x00AD5C40`; +4 / +8 the object vector by id (ids 1..16 the heroes). Objects: +0x0C id, +0x10 type, +0x14 flags (0x10 freed or parked), +0x18 position `{u16 sector, i32 x, i32 y, u8 layer}`. Creatures: +0x3B4 experience, +0x3EE gold, +0x3FE level, +0x4D4 / +0x4D8 maximum / current health. |
| `cObjectManager::create` 0x005FBA40 | thiscall (type, position*, 0, broadcast, x) -> id; the hero is created with (type, pos, 0, 1, 0), the script command CreateNPC with (type, pos, 0, 0, x). |
| Party | The players object `*0x0182EBE8` +0x14 is the local hero id; an object ticks only where its owner byte (+0x39) is the local id. Switching control (gear/party.cpp) rewrites the owner of everything the old id owned, sets +0x14 and the engine's mouse target (`*0x00AD6EF8` +0x13C; a savegame load resets +0x14 to 1 but not this), swaps the follower vector (+0x39C, 9-byte entries, mercenary leader at +0x251) and rewrites the anti-cheat shadows (`*0x0182EE5C` and `*0x0182EE60`, checked by 0x00590730 against `hero()`: experience, gold, level XOR the hero's byte +0x38 x4 (doubled for the second page), then the dwords from +0x3B0). A hero is created with `create(type 1..9, pos, id, 0, 0)` and finished as 0x0060D9F0 does (+0x18 copied to +0x2B9, 0x0055FC00 (stats, 0, -1)). Stats block +0x3A8: abilities vector at +0x102 (0x16-byte entries: +4 id, +6 level, +7 bonus, +0xA regeneration time, +0x12 seconds left), experience gain 0x0057E160 (stats, amount) levels up. Orders are 0x44-byte events (vtable 0x0089095C) through the creature's vtable +0x18: move type 4 (+0x14 mode 2, +0x18 x, +0x1C y), attack 7 (+0x18 target, +0x1C x, +0x20 y), ability 0x105 (+0x10 target, +0x14 sector, +0x18 x, +0x1C y, +0x28 100, +0x2C 1 art / 0 spell, +0x30 id, +0x34 layer). 0x006107BC drives object 2 with the arrow keys in single player. |
| `cObjectManager::pop` 0x005FCCB0 | thiscall (id, position*): places an object (teleport); ParkCreature (0x006073D0) moves one far away with it. |
| Object types | `*0x00AAB5E4` + id * 0x80 (0 < id < 0x7E60): +0x2E kind, +0x37 name. |
| Saving | The savegame window (0x0071CC90 action 2) sends a 0x24-byte event (vtable 0x00890B38, +4 code 0xF, +8 sub 2, +0xC slot, +0x10 stored in the header, +0x14 capture the picture first, +0x18 check that no trade or chest window blocks (the engine logs AUTOSAVE and refuses otherwise), +0x20 the display name, copied into the engine's 64 bytes at +0x6A) to `cEngine::receiveEvent` (0x00618090, thiscall (event)). It stores the slot at engine +0x68 and sets 0x40 in the engine's flags (+0x54), unless that bit is already set. The render thread loop (0x0060E350) writes `SAVE\GAME%02d.PAK` (0x00619BF0) when `flags & 0xC0` and clears them; 0x200 is a queued load. Byte `*0x0182EBE8` +4 is 0 in single player. |
| Inventories | `*0x00AD4448`: the inventory manager (made by 0x005DEBC0), 32 containers of 0x1180 bytes (0x005DEC90 (index) returns one). Container: +0x18 + cell * 12 the cells, +0x115C columns, +0x115E rows, +0x1160 its number; cell = row * columns + column: dword item id on an item's top-left cell (0 on its other cells), byte +4 flags (bit 0 taken), +5 / +6 the item's width / height there, on its other cells the column / row of its top-left cell. Container n (1..16) is the backpack of the hero with object id n; 17 the chest (`UI_WND_CHEST`, 20 x 10, made by 0x006ACB40); 18, 25..27 are small grids (3 x 5, 3 x 5, 7 x 5, 7 x 5) whose use is not examined; 20..22 hold the shops' stock in the test savegame. `0x005DABA0` (thiscall (w, h, slot*, byRow) -> bool) finds room, `0x005D9F00` (slot, id, w, h, notify) puts an item in (takes it out of the world: flag 2 at +0x14; tells the item its owner), `0x005DA630` (id, notify) takes it out of the container (the object stays), `0x00428D60` (stdcall (type, w*, h*)) is a type's size in cells, `0x005FBDB0` (id, 1, 0, 0) destroys an object (the engine does this after taking an item out, e.g. on using a potion, 0x005BFF60). An item (cItemBase) keeps its own state in a vector of 0x82-byte entries (+0x100 begin, +0x104 end, +0x108 capacity), 0x82 bytes at +0x118 (the record copy: class mask and rarity at +0x11C) and a number made from them at +0x120 (0x0042BF20 (record) -> hash, set after reading an item, 0x005C7690); the network read of an item (vtable slot 8, 0x005C7A50) restores exactly the entries and the record. Gear copies those to bytes (type, count, entries, record) and rebuilds an item by creating one of the type (0x005FBA40) and overwriting them (growing the vector with 0x005C9230). |
| Item tooltips | The equipment window's hover (0x006B7700) takes the UI manager's reserved popup (u16 at manager +0x54, `0x0075B1E0` (index) -> cUI_Popup*), builds the text with the item's virtual function 17 (thiscall (std::vector<wchar_t>* out, record or 0, 0, 0x5F), ret 0x10; the vector's buffer comes from `0x008321A0` and is freed with `0x0041D3B0` (ptr, count)), then `0x006E8860` (0xA: clamp to the screen and `x` / `y` are the top-left corner), `x` / `y` at popup +0x24 / +0x28, `0x006E6EA0` (0, vector*, 0) sets the text (SacredBild detours its first five bytes: Gear finds it by its body and calls entry = body - 0x36), `0x00732550` (1) shows it. The popup's size from its last layout is at +0x2C / +0x2E. Weapons (cWeapon3D, slot 8 = `0x005CF2D0`) write four dwords (+0x19C, +0x1AC, +0x1A0, +0x1BC) after the common item data; socketing is `0x005BFF60` (item, upgrade id, socket) -> bool. |
| Game Auto-Save | `Settings.cfg` `AUTOSAVE : 1` (options window UI_CFG_AUTOSAVE, 0x0071B5FA; the options map at `0x182EE88` holds the keys, the getter 0x0081B6F0 returns the AUTOSAVE flag). The script interpreter's command 0x79 (0x00475680 case 0x79 -> 0x004AC740) takes the value of the script's last parameter (`+0xA880`, into engine+0xAC, the save header), and if AUTOSAVE is on and `*0x0182EBEC` is 0 (single player) sends event 0xF sub 3 (capture the picture) and then sub 2 with slot 0xFFFFFFFF (the first free `GAMEnn` from 1, 0x00619B80) and +0x18 = 0 (no window check, no name). Saves nothing if bit 0x40 of engine+0x54 is already set. |
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