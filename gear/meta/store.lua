---@meta _
-- Sacred Gear: a mod's data in JSON files (gear.store).

-- ## Stored data (gear.store)
--: A mod keeps its own data (a log, totals, saved lists) in JSON files of its folder (`gear.read` / `gear.save`). `load` and
--: `save` do the reading, decoding, encoding and writing with defaults and one log line per broken file; `file` loads a table
--: once and saves it when it changed, every few seconds and when the mod unloads. The library is shipped in the DLL
--: (`gear/lua/store.lua`) and runs once per mod, the first time the mod uses `gear.store`.

--- The options of `file`.
---@class GearStoreOpts
---@field every? number Seconds between saves of a changed file (default 30; 0: only on unload and `save`).
---@field check? fun(data: any): any Repairs or replaces what was loaded (called once).
---@field value? fun(): any What is written (default `handle.data`).

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
