---@meta _
-- Sacred Gear: the `gear` global every mod gets, its events and the mod manifest.
-- LuaLS definitions, generated into docs/GearApi.md by tools/gen_gear_api_docs.py and checked against
-- gear/lua/kernel.lua by tools/check_gear_api.py. Format: docs/GearApi.md, "Maintaining the annotations".

-- ## The gear global

---@alias GearEventName
---| 'load' # All mods are loaded.
---| 'frame' # Every in-game frame.
---| 'draw' # After the frame handlers, to draw the UI.
---| 'idle' # Automation only: every 50 ms in the menus.
---| 'unload' # The player switched this mod off in the mod manager: clean up (handlers, timers and the rest are dropped after it).
---| 'settings.changed' # A setting of this mod changed: `(id, value)`.
---| 'session.start' # A game with a hero runs.
---| 'session.stop' # That hero is gone.
---| 'game.saved' # A save `gear.game.save` asked for is written.
---| 'game.save_failed' # It could not be written.
---| 'game.saved_other' # A save the game or the player made: the quicksave, the savegame window, the game's own Auto-Save.
---| 'game.hero_saved' # The game wrote the hero file (`Save\HeroNN.pax`): `(slot, network)`. A network game does this every 5 minutes, when the player leaves and at merchants; the moment an item or gold that changed hands is safe in the player's own save.
---| 'kill' # A kill the local computer sent to the game (a hero's kill: `(kill)`, see `GearKill`).
---| 'mp_message' # A message of the mod channel (`gear.mp.send`) arrived: `(from, channel, payload)`.
---| 'net_caps' # The custom net messages became available, or the list of players who can use them changed (gear.net): `(caps)`, see `GearNetCaps`.
---| 'realm.changed' # `gear.realm.use` switched the online play server: `(realm)`.
---| 'combat.constant' # The engine loads a pooled float constant inside its own code (hit handlers, difficulty factors ...); a handler may return another number for it.
---| 'skillslot.key' # A character key reached the skill slot window; a handler routes it to a slot (`slot` or `{slot, 'select'}`), `false` ignores it.
---| 'combat.branch' # A conditional jump in the hit handlers and a few other functions: a handler may decide whether it is taken (`name`, `vanilla`, `index`, the compared `eax`).
---| 'skillslot.select' # A digit key selected a skill slot of the HUD (`index`: which of the five places in the key handler).
---| 'bonus.constant' # A number the item bonus code pushes as the argument of a random roll or a text lookup; a handler may return another.
---| 'drops.roll' # A creature's loot function is about to run; a handler may change the rarity bonus or return false to cancel the roll.
---| 'drops.select' # The loot function asks the equipment type picker for an item; a handler may change the category, level or quality, or name the item type.
---| 'effect.add' # The engine adds a value to a creature's effect entry; a handler may decide what is stored (see `GearEffectAdd`).
---| 'damage' # The final damage of a hit, before the engine applies it; a handler may return another damage (16 bit, 0 to 65534).
---| 'creature.ability' # The creature AI picks the ability (skill or spell id) a creature uses; a handler may return another id.
---| 'level.factor' # The engine asks for the integer factor of a level (a hook inside its own code; the first handler that returns a number answers).

--- A handler registered with `gear.on`; pass it to `gear.off` to remove it.
--- What the `damage` event gets.
---@class GearHit
---@field damage integer The damage the engine computed (after the resistances, before it is applied).
---@field victim_type integer Creature type of the one that is hit (-1: unknown).
---@field attacker_type integer Creature type of the attacker (-1: none, e.g. a trap or an effect).
---@field site integer Which damage hook site (0: the damage resolution of the hit handlers).

--- What the `effect.add` event gets: the engine is adding to one entry of a creature's effect list.
---@class GearEffectAdd
---@field creature integer The creature's id.
---@field ability integer The ability the creature is using (its current skill or spell id).
---@field effect integer The effect id of the entry the engine is at.
---@field flags integer The creature's flag word (bits 0x4000000 and 0x20000000 mark stacking effects).
---@field counter integer The creature's effect counter.
---@field value number The new sum (the entry's value plus what is added).
---@field max number The entry's maximum.
---@field current number The entry's value now.

--- What a handler of `effect.add` answers; nil means the engine does its own store and clamps.
---@class GearEffectAnswer
---@field value? number Store this in the entry; without it nothing is stored.
---@field effect? integer Change the next entry (from this one on) with this effect id instead of the engine's.
---@field clamp? boolean Apply the engine's clamps (default true): above the maximum becomes the maximum, below its floor becomes 0.
---@field finish? boolean Leave the function after this entry (default false: go on with the next).
---@field stack? 'set'|'inc' The creature's effect counter becomes 1 / grows by one.

---@class GearHandler
---@field event string The event the handler listens to.
---@field fn function The handler.

--- A timer started with `gear.after` or `gear.every`; pass it to `gear.cancel`. Opaque.
---@class GearTimer

--- What `gear.mod` tells a mod about itself.
---@class GearModInfo
---@field id string The manifest's `id`.
---@field name string The manifest's `name` (default: the id).
---@field version string The manifest's `version` (default `'0'`).
---@field dir string The mod's folder name under `mods`.

-- path gear
--- The `gear` global: Sacred Gear's API, one copy per mod (each mod has its own globals).
---
--- All mod code runs on the engine thread, between the game's update and the drawing of the frame, one handler at a
--- time, so handlers see a consistent state and need no locks.
---@class Gear
---@field version string Sacred Gear's version.
---@field mod GearModInfo This mod's identity.
---@field config table<string, any> This mod's settings: the manifest's `config`, overridden by `config.lua`.
---@field data GearData The engine's data tables (read only, except the abilities).
---@field text GearText The game's texts by id.
---@field files GearFiles The mod's file overlay.
---@field hooks GearHooks The engine hooks that ask mods for values.
---@field json GearJson JSON encoding and decoding.
---@field pipe GearPipeModule Named pipe clients.
---@field world GearWorld The world: hero, creatures, items, spawning, pickup rules.
---@field drops GearDrops Item drops: chance, rarity weights, the generator, the roll and select events.
---@field inv GearInv Inventories: containers, items, export and import of items.
---@field game GearGame The game itself: difficulty and saving.
---@field clipboard GearClipboard The Windows clipboard: copy text out of the game.
---@field realm GearRealmApi The online play server (realms): list, switch, add.
---@field mp GearMp Players of a network game, the host, the mod channel and the `kill` event.
---@field net GearNet Custom net messages: named channels between computers and the game server.
---@field settings GearSettings This mod's settings (declared in `mod.lua`, changed by the player in the mod manager).
---@field keys GearKeys This mod's declared key binds.
---@field mods GearMods The installed mods; a page of this mod's own in the mod manager.
---@field ui GearUi Drawing on top of the game, widgets, panels and the layout.
---@field auto? GearAuto Automation (only with `-gear-auto` and `automation = true` in `mod.lua`).
gear = {}

--- Calls `fn` for `event`; returns a handle for `gear.off`. Built-in events: see `GearEventName`. Any other name is a
--- custom event that `gear.emit` fires (it has a dot: `twitch.command`).
---@overload fun(event: 'load', fn: fun()): GearHandler
---@overload fun(event: 'frame', fn: fun(dt: number)): GearHandler
---@overload fun(event: 'draw', fn: fun(state: GearUiState)): GearHandler
---@overload fun(event: 'idle', fn: fun()): GearHandler
---@overload fun(event: 'unload', fn: fun()): GearHandler
---@overload fun(event: 'settings.changed', fn: fun(id: string, value: boolean|number|string)): GearHandler
---@overload fun(event: 'session.start', fn: fun(hero: Creature)): GearHandler
---@overload fun(event: 'session.stop', fn: fun()): GearHandler
---@overload fun(event: 'game.saved', fn: fun(slot: integer, info: GearSaveInfo)): GearHandler
---@overload fun(event: 'game.save_failed', fn: fun(slot: integer, message: string)): GearHandler
---@overload fun(event: 'game.saved_other', fn: fun(slot: integer)): GearHandler
---@overload fun(event: 'game.hero_saved', fn: fun(slot: integer, network: boolean)): GearHandler
---@overload fun(event: 'kill', fn: fun(kill: GearKill)): GearHandler
---@overload fun(event: 'mp_message', fn: fun(from: GearPlayer, channel: string, payload: table|string)): GearHandler
---@overload fun(event: 'net_caps', fn: fun(caps: GearNetCaps)): GearHandler
---@overload fun(event: 'realm.changed', fn: fun(realm: GearRealm)): GearHandler
---@overload fun(event: 'combat.constant', fn: fun(name: string, vanilla: number, index: integer): number?): GearHandler
---@overload fun(event: 'skillslot.key', fn: fun(character: integer): (integer|false|{[1]: integer, [2]: 'full'|'select'})?): GearHandler
---@overload fun(event: 'combat.branch', fn: fun(name: string, vanilla: boolean, index: integer, eax: integer): boolean?): GearHandler
---@overload fun(event: 'skillslot.select', fn: fun(index: integer)): GearHandler
---@overload fun(event: 'bonus.constant', fn: fun(index: integer, vanilla: number, address: integer): number?): GearHandler
---@overload fun(event: 'drops.roll', fn: fun(ev: GearDropRoll): boolean?): GearHandler
---@overload fun(event: 'drops.select', fn: fun(ev: GearDropSelect)): GearHandler
---@overload fun(event: 'effect.add', fn: fun(e: GearEffectAdd): GearEffectAnswer?): GearHandler
---@overload fun(event: 'damage', fn: fun(hit: GearHit): integer?): GearHandler
---@overload fun(event: 'creature.ability', fn: fun(type: integer, vanilla: integer, index: integer, address: integer): integer?): GearHandler
---@overload fun(event: 'level.factor', fn: fun(level: number, site: integer): number?): GearHandler
---@param event string
---@param fn fun(...: any)
---@return GearHandler
function gear.on(event, fn) end

--- Removes a handler.
---@param handle GearHandler
function gear.off(handle) end

--- Calls every handler of a custom event now. Custom names have a dot (`twitch.command`); the built-in events and the
--- prefixes `session.`, `gear.`, `pipe.`, `game.` and `settings.` are reserved. Handlers may emit events too, up to 8 levels deep.
---@param event string
---@param ... any
function gear.emit(event, ...) end

--- Runs `fn` once after `seconds` (real seconds while the game runs frames, not in menus or loading screens).
---@param seconds number
---@param fn fun()
---@return GearTimer
function gear.after(seconds, fn) end

--- Runs `fn` every `seconds`. A timer whose function raised an error stops. At most 1000 timers per mod.
---@param seconds number Greater than 0.
---@param fn fun()
---@return GearTimer
function gear.every(seconds, fn) end

--- Stops a timer.
---@param timer GearTimer
function gear.cancel(timer) end

--- Runs `fn` as a task: a coroutine that may wait with `gear.wait` and `gear.wait_until` without holding up the frame.
--- It is resumed every tick (in game every frame; with automation in the menus too).
---@param fn fun()
function gear.task(fn) end

--- Inside a task: waits `seconds`.
---@param seconds number
function gear.wait(seconds) end

--- Inside a task: waits until `test()` returns true, at most `timeout` seconds.
---@param test fun(): boolean
---@param timeout? number Default 30.
---@return boolean satisfied True when `test()` returned true, false after the timeout.
function gear.wait_until(test, timeout) end

--- A line in `SacredGear.log`, prefixed with the mod's id. Same as `print`.
---@param ... any
function gear.log(...) end

--- Seconds, high resolution, from an arbitrary start.
---@return number
function gear.time() end

--- Reads a file of the mod's own folder (directly in it, no subfolders).
---@param name string
---@return string? text
---@return string? error
function gear.read(name) end

--- Writes a file in the mod's folder, for settings a mod's own UI changes: `*.json`, `*.txt` or `*.lua` other than
--- `mod.lua` and `main.lua`, at most 8 MB. The write is atomic: the text goes to `<name>.tmp`, is flushed
--- and then replaces the file, so a crash leaves the old file or the new one, never part of one.
---@param name string
---@param text string
---@return boolean? ok
---@return string? error
function gear.save(name, text) end

-- ## The manifest (mod.lua)

--- The table `mod.lua` returns. It runs without any globals, so it can only hold values. Annotate it with
--- `---@type GearModManifest` above the `return`.
---@class GearModManifest
---@field id string Required: letters, digits, `_` and `-`; unique among the installed mods.
---@field name? string Shown in the mod manager and the log (default: the id).
---@field description? string One or two lines for the mod manager.
---@field author? string Shown in the mod manager.
---@field version? string Part of the mod set (default `'0'`).
---@field priority? number Lower loads first (default 0); ties load by id.
---@field requires? string[] Ids that must load first; the mod is skipped if one is missing.
---@field enabled? boolean `false`: off until the player switches it on in the mod manager (the default, not a lock).
---@field settings? GearSettingDef[] Declared settings: the mod manager builds their page; read them with `gear.settings`.
---@field keys? GearKeyDef[] Declared key binds: the player rebinds them in the mod manager; use them with `gear.keys`.
---@field config? table<string, any> Default settings: `gear.config` starts as a copy and `config.lua` overrides them.
---@field pipes? string[] Names of the named pipes the mod may open (`gear.pipe.open`).
---@field automation? boolean `true`: the mod gets `gear.auto` when the game runs with `-gear-auto`.

-- ## The standard library a mod gets

-- path os
--- A mod gets `string`, `table`, `math`, `bit`, `coroutine` and the basic functions below, and from `os` only `clock`,
--- `time`, `date` and `difftime`. Each mod has its own globals.
---@class GearOs
---@field clock fun(): number
---@field time fun(t?: table): integer
---@field date fun(format?: string, time?: integer): string|table
---@field difftime fun(t2: integer, t1: integer): number
os = {}

--- Runs `name.lua` of this mod's folder once (`a.b` is `a\b.lua`) and returns its result (`true` if it returns
--- nothing). Only the mod's own files.
---@param name string
---@return any
function require(name) end

--- A line in `SacredGear.log`, prefixed with the mod's id. Same as `gear.log`.
---@param ... any
function print(...) end

-- The rest of LuaJIT's standard library is not in a mod's environment. Set runtime.builtin as in
-- meta/luarc.template.json to hide the libraries (io, debug, package, jit, ffi, ...); these basic functions cannot be
-- hidden by a setting, so they are marked here.

---@deprecated Not available to Sacred Gear mods.
function load(chunk, chunkname, mode, env) end
---@deprecated Not available to Sacred Gear mods.
function loadstring(text, chunkname) end
---@deprecated Not available to Sacred Gear mods.
function loadfile(filename) end
---@deprecated Not available to Sacred Gear mods.
function dofile(filename) end
---@deprecated Not available to Sacred Gear mods.
function collectgarbage(opt, arg) end
---@deprecated Not available to Sacred Gear mods.
function getfenv(f) end
---@deprecated Not available to Sacred Gear mods.
function setfenv(f, table) end
---@deprecated Not available to Sacred Gear mods.
function newproxy(arg) end
---@deprecated Not available to Sacred Gear mods.
function gcinfo() end
