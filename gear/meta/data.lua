---@meta _
-- Sacred Gear: the engine's data tables, JSON and pipes.

-- ## Data tables (gear.data)

-- path gear.data
--- The engine's data tables, read only.
---@class GearData
---@field object_types GearObjectTypes The type records every object refers to (0x80 bytes; id = index, 1..0x7E5F).
---@field creature_types GearTable `Pak\Creature.pak`'s records (0x56 bytes).
local GearData = {}

-- path record
--- One record of a table, read in place. Fields are read by byte offset; nil if out of range.
---@class GearRecord
---@field index integer The record's index.
local GearRecord = {}

---@param offset integer
---@return integer?
function GearRecord:u8(offset) end
---@param offset integer
---@return integer?
function GearRecord:i8(offset) end
---@param offset integer
---@return integer?
function GearRecord:u16(offset) end
---@param offset integer
---@return integer?
function GearRecord:i16(offset) end
---@param offset integer
---@return integer?
function GearRecord:u32(offset) end
---@param offset integer
---@return integer?
function GearRecord:i32(offset) end
---@param offset integer
---@return number?
function GearRecord:f32(offset) end

--- A zero-terminated string.
---@param offset integer
---@param max_length? integer Default 64.
---@return string?
function GearRecord:str(offset, max_length) end

-- path records
--- A read only table of records.
---@class GearTable
local GearTable = {}

--- The number of records (0 if the table is not available).
---@return integer
function GearTable:count() end

--- The size of a record in bytes (0 if the table is not available).
---@return integer
function GearTable:stride() end

--- Record `index`, counted from 0.
---@param index integer
---@return GearRecord? record nil if out of range.
function GearTable:get(index) end

-- path gear.data.object_types
--- The object types. Besides the table functions it knows the types by name.
---@class GearObjectTypes : GearTable
local GearObjectTypes = {}

--- The internal name of type `id` (nil if it has none).
---@param id integer
---@return string?
function GearObjectTypes:name(id) end

--- The kind (the type record's byte +0x2E) of type `id`.
---@param id integer
---@return integer?
function GearObjectTypes:kind(id) end

--- The id of the type named `name` (any case; the first type with that name).
---@param name string
---@return integer?
function GearObjectTypes:find(name) end

--- The types whose name contains `text` (plain text, any case), ordered by id.
---@param text string
---@param limit? integer Default 100.
---@return GearTypeMatch[]
function GearObjectTypes:search(text, limit) end

---@class GearTypeMatch
---@field id integer
---@field name string?
---@field kind integer?

-- ## JSON (gear.json)

-- path gear.json
--- Objects are tables with string keys, arrays tables with the keys 1..n.
---@class GearJson
---@field null table Stands for JSON null (`decode` gives it for null values in arrays and objects).
local GearJson = {}

--- Encodes a value as JSON text. Errors for functions, NaN, infinity and keys that are not strings.
---@param value any
---@return string
function GearJson.encode(value) end

--- Decodes JSON text.
---@param text string
---@return any? value
---@return string? error
function GearJson.decode(text) end

-- ## Named pipes (gear.pipe)

-- path gear.pipe
--- Named pipe clients.
---@class GearPipeModule
local GearPipeModule = {}

--- A client of `\\.\pipe\<name>`; the name must be listed in `pipes` in `mod.lua`. The client reconnects by itself.
--- Messages are framed with a 16-bit little-endian length (the risen-twitch / Gothic Twitch connector's format).
---@param name string
---@return GearPipe
function GearPipeModule.open(name) end

-- path pipe
--- A pipe client. Set `on_message`, `on_connect` and `on_disconnect` to receive.
---@class GearPipe
---@field name string
---@field on_message? fun(text: string) Called for each message received.
---@field on_connect? fun() Called when the connection is made.
---@field on_disconnect? fun() Called when it is lost.
local GearPipe = {}

--- Sends a message; false if the pipe is closed or not connected.
---@param text string
---@return boolean
function GearPipe:send(text) end

---@return boolean
function GearPipe:connected() end

--- Closes the pipe for good.
function GearPipe:close() end
