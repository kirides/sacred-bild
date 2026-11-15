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
---| 'session.start' # A game with a hero runs.
---| 'session.stop' # That hero is gone.
---| 'level.factor' # The engine asks for the integer factor of a level (a hook inside its own code; the first handler that returns a number answers).

--- A handler registered with `gear.on`; pass it to `gear.off` to remove it.
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
---@field settings GearSettings The game's Settings.cfg.
---@field files GearFiles The mod's file overlay.
---@field hooks GearHooks The engine hooks that ask mods for values.
---@field json GearJson JSON encoding and decoding.
---@field pipe GearPipeModule Named pipe clients.
---@field world GearWorld The world: hero, creatures, items, spawning, pickup rules.
---@field ui GearUi Drawing on top of the game, widgets, panels and the layout.
---@field auto? GearAuto Automation (only with `-gear-auto` and `automation = true` in `mod.lua`).
gear = {}

--- Calls `fn` for `event`; returns a handle for `gear.off`. Built-in events: see `GearEventName`. Any other name is a
--- custom event that `gear.emit` fires (it has a dot: `twitch.command`).
---@overload fun(event: 'load', fn: fun()): GearHandler
---@overload fun(event: 'frame', fn: fun(dt: number)): GearHandler
---@overload fun(event: 'draw', fn: fun(state: GearUiState)): GearHandler
---@overload fun(event: 'idle', fn: fun()): GearHandler
---@overload fun(event: 'session.start', fn: fun(hero: Creature)): GearHandler
---@overload fun(event: 'session.stop', fn: fun()): GearHandler
---@overload fun(event: 'level.factor', fn: fun(level: number, site: integer): number?): GearHandler
---@param event string
---@param fn fun(...: any)
---@return GearHandler
function gear.on(event, fn) end

--- Removes a handler.
---@param handle GearHandler
function gear.off(handle) end

--- Calls every handler of a custom event now. Custom names have a dot (`twitch.command`); the built-in events and the
--- prefixes `session.`, `gear.` and `pipe.` are reserved. Handlers may emit events too, up to 8 levels deep.
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
--- `mod.lua` and `main.lua`, at most 1 MB.
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
---@field name? string Shown in the log (default: the id).
---@field version? string Part of the mod set (default `'0'`).
---@field priority? number Lower loads first (default 0); ties load by id.
---@field requires? string[] Ids that must load first; the mod is skipped if one is missing.
---@field enabled? boolean `false`: keep the folder, skip the mod.
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
