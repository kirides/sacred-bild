---@meta _
-- Sacred Gear: a mod's data in JSON files (gear.store).

-- ## Stored data (gear.store)
--: A mod keeps its own data (a log, totals, saved lists) in JSON files of its folder (`gear.read` / `gear.save`). `load` and
--: `save` do the reading, decoding, encoding and writing with defaults and one log line per broken file; `file` loads a table
--: once and saves it when it changed, every few seconds and when the mod unloads. The library is shipped in the DLL
--: (`gear/lua/store.lua`) and runs once per mod, the first time the mod uses `gear.store`.
--:
--: What belongs to one character or savegame (progress, lifetime totals, a log) goes in `gear.store.game` instead of `file`: it
--: is kept per savegame (see `gear.savegame`), so it does not leak from one savegame or character into another. Settings, window
--: places and what all games share stay in `file` / `load` / `save`.

--- The options of `file`.
---@class GearStoreOpts
---@field every? number Seconds between saves of a changed file (default 30; 0: only on unload and `save`).
---@field check? fun(data: any): any Repairs or replaces what was loaded (called once).
---@field value? fun(): any What is written (default `handle.data`).

--- The options of `game` (those of `file`, and):
---@class GearStoreGameOpts: GearStoreOpts
---@field on_change? fun(handle: GearStoreFile, kind: 'load'|'hero'|'new', scope: string?) Called after the handle holds the data of another game (the mod's `savegame.changed` handlers run after it).
---@field migrate? boolean A file `name` of the mod's folder from before the mod used per game data is taken once, by the first loaded savegame that has no data yet; `<name>.migrated.txt` records it.

--- A file of `gear.store.file`.
---@class GearStoreFile
---@field name string
---@field data any The loaded table (or the default); change it, then call `changed`.
---@field dirty boolean
local GearStoreFile = {}

--- Marks the data as changed: it is written at the next save.
function GearStoreFile:changed() end

--- Writes the file now.
---@return boolean ok
---@return string? error
function GearStoreFile:save() end

--- Writes the file if it changed.
---@return boolean ok
---@return string? error
function GearStoreFile:flush() end

-- path gear.store
--- JSON files of the mod's folder.
---@class GearStore
local GearStore = {}

--- The decoded file, or `default` when it is missing, not JSON, or not a table while `default` is one. The second result is
--- `'missing'` or `'invalid'` when the default was taken.
---@generic T
---@param name string
---@param default T
---@return T data
---@return ('missing'|'invalid')? why
function GearStore.load(name, default) end

--- Writes `value` as JSON. The first failure of each file is logged.
---@param name string
---@param value any
---@return boolean ok
---@return string? error
function GearStore.save(name, value) end

--- Loads `name` (or takes `default`) and keeps it: the handle saves it when it changed (see `GearStoreOpts`) and on unload.
---@param name string
---@param default any
---@param opts? GearStoreOpts
---@return GearStoreFile
function GearStore.file(name, default, opts) end

--- A file of the savegame being played: what the mod keeps about this character or game. The same handle as `file`, but the
--- data belongs to the savegame: saving the game into a slot keeps it for that slot, loading a slot brings back what was kept for
--- it (the defaults if nothing was), a new game starts from `default`. Progress made since the last save is dropped when another
--- game loads, as the game's own progress is, and `handle.data` is replaced then: always reach the data through the handle
--- (`handle.data.x`), never keep a part of it. Files are in `saves/<scope>/` of the mod's folder.
---@param name string
---@param default any
---@param opts? GearStoreGameOpts
---@return GearStoreFile
function GearStore.game(name, default, opts) end
