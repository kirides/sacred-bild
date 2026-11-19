---@meta _
-- Sacred Gear: the engine's data tables, JSON and pipes.

-- ## Data tables (gear.data)

-- path gear.data
--- The engine's data tables: read only, except the abilities.
---@class GearData
---@field object_types GearObjectTypes The type records every object refers to (0x80 bytes; id = index, 1..0x7E5F).
---@field creature_types GearTable `Pak\Creature.pak`'s records (0x56 bytes).
---@field abilities GearAbilities The ability tables of the exe by id (skills, combat arts, spells), which a mod can change.
---@field regions GearRegions The engine's region kill countdowns (the Region Boss quest), which a mod can read and change.
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

--- The localized name of type `id`: for an item as the game's tooltip shows it (the first line, colour codes removed; made from a
--- scratch item, nothing is registered or sent), for a creature (kind 3) the game's text for its type (nil for the few creature
--- types the game gives no name text, and for types that are neither; fall back to `name`). The names are cached.
---@param id integer
---@return string? name
---@return string? error
function GearObjectTypes:display_name(id) end

--- What to show the player for type `id`: `display_name`, else `name`, else `'type <id>'`. Never nil.
---@param id integer
---@return string
function GearObjectTypes:label(id) end

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

-- ## Abilities (gear.data.abilities)

-- path gear.data.abilities
--- The exe's two ability tables as one table by id: T1 (the combat arts: ids from 1000, 0x78-byte records) and T2 (the spells: ids
--- from 1, 0x94-byte records). `abilities[id]` is the record of ability `id` (nil if there is none); its fields read and write by name.
--- A field has a name where code that uses it proves what it is, and is `unk_<offset>` (in hex) otherwise. Writes go to the
--- live tables, are checked for type and range, and are undone with `restore` (and when the mod is switched off).
---@class GearAbilities
local GearAbilities = {}

--- The ids of all abilities, ascending.
---@return integer[]
function GearAbilities:ids() end

--- The fields of the table that ability `id` is in, ordered by offset.
---@param id integer
---@return GearAbilityField[]? fields nil if there is no such ability.
function GearAbilities:fields(id) end

--- Writes many fields at once: `{[id] = {field = value, ...}}`. Everything is checked first; if any id, field or value is
--- wrong nothing is written and the problems are returned.
---@param edits table<integer, table<string, number>>
---@return integer? written The number of fields written (nil if nothing was).
---@return string[]? problems What was wrong.
function GearAbilities:apply(edits) end

--- Undoes this mod's writes to the ability tables.
---@return integer restored The number of fields restored.
function GearAbilities:restore() end

---@class GearAbilityField
---@field name string
---@field offset integer The byte offset in the record.
---@field kind string `'u8'`, `'i16'`, `'f32'` ...

-- path ability
--- One ability record. Reading a field of a table the ability is not in (or an unknown name) gives nil; writing one is an
--- error, as is writing a value that does not fit the field or the `id`. Both have `id`, `name_text`, `description_text`,
--- `target_mode` and the `unk_` fields; T1 also `effect_base`, `effect_per_level`, `effect2_base` and `effect2_per_level`; T2 also
--- `value0_base` .. `value3_per_level`.
---@class GearAbility
---@field table string `'T1'` or `'T2'`.
---@field index integer The record's index in its table.
---@field kind string `'combat_art'` (T1: ids from 1000) or `'spell'` (T2: ids from 1): what the names in the game's texts say they are.
---@field id integer The ability's id (a copy's own id).
---@field base integer The id of the ability whose code runs (a copy's base; the game's own abilities: their own id).
---@field name_text integer The text id of the name (`gear.text.get`).
---@field description_text integer The text id of the description.
---@field target_mode integer How the ability is aimed (the engine passes it on to the targeting code).
---@field effect_base number T1: the first effect is `effect_base + level * effect_per_level` (level 1..100).
---@field effect_per_level number T1.
---@field effect2_base number T1: the second effect.
---@field effect2_per_level number T1.
---@field value0_base integer T2: the first value is `value0_base + level * value0_per_level`.
---@field value0_per_level integer T2.
---@field value1_base integer T2.
---@field value1_per_level integer T2.
---@field value2_base integer T2.
---@field value2_per_level integer T2.
---@field value3_base integer T2.
---@field value3_per_level integer T2.
local GearAbility = {}

-- ## Game text (gear.text)

-- path gear.text
--- The game's own texts, by the text ids its data tables hold (not the keys of `global.res`).
---@class GearText
local GearText = {}

--- The text for `id` (UTF-8), nil if the game has none.
---@param id integer
---@return string?
function GearText.get(id) end

--- Replaces the text of an id the game has (a text is a string or `{en = ..., de = ..., ru = ...}`: the game's language, else
--- `en`, else any). Ids from 0x100000 are Gear's own (`add`). Ids the game does not have are refused.
---@param id integer
---@param text string|table<string, string>
---@return boolean? ok
---@return string? error
function GearText.set(id, text) end

--- Makes a new text of Gear's own and returns its id (from 0x100000), which the game's text lookup serves: for the
--- `name_text` and `description_text` of ability copies.
---@param text string|table<string, string>
---@return integer? id
---@return string? error
function GearText.add(text) end

-- ## Files (gear.files)

-- path gear.files
--- The file overlay: a mod's own data files served where the game opens the original ones, and the mod's own save folder.
--- Declared in `mods/<id>/overlay.txt` (`game\path = data\file` per line, `saves = save`), read when Sacred Gear starts
--- because the game opens its data files before any Lua runs. The rules and the patch formats: [Gear.md](Gear.md#replacing-game-files-file-overlay).
---@class GearFiles
local GearFiles = {}

--- What this mod's overlay does, nil if the mod has no `overlay.txt` (or the run does not use it).
---@return GearOverlayInfo?
function GearFiles.overlay() end

--- The path the game's open of `path` (relative to the game folder) ends up at: the mod's file if an overlay rule applies,
--- else `path` unchanged.
---@param path string
---@return string
function GearFiles.resolve(path) end

--- How many times the game opened `path` (relative to the game folder) and was served a mod's file by the overlay.
---@param path string
---@return integer
function GearFiles.served(path) end

---@class GearOverlayInfo
---@field files integer The number of file rules.
---@field saves string? The mod's save folder (absolute) if the game's `Save\` folder is redirected to it.

-- ## Engine hooks (gear.hooks)

-- path gear.hooks
--- The engine hooks that ask mods for a value inside the game's own code (see the events `level.factor`).
---@class GearHooks
local GearHooks = {}

--- The counters of a hook since the game started: how often the engine asked, how often a mod answered, and how often
--- Lua could not take the question (busy: it was running on this or another thread, so the engine kept its own value).
---@param name 'level.factor'|'combat.constant'|'creature.ability'|'damage'|'effect.add'|'bonus.constant'|'skillslot.select'|'combat.branch'|'skillslot.key'
---@return GearHookStats
function GearHooks.stats(name) end

--- Every hook site and how often the engine ran it (a site whose function was not found or whose code did not match is listed
--- with address 0).
---@return GearHookSite[]
function GearHooks.sites() end

---@class GearHookSite
---@field hook string The hook's name (`level.factor`, or the name of a `combat.constant`).
---@field index integer The place among those of that name.
---@field address integer Where the site is in the game.
---@field calls integer How many times the engine ran the site.

---@class GearHookStats
---@field calls integer
---@field answered integer
---@field busy integer

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

-- path regions
--- One region of the kill countdown: when `kills_left` reaches 0 the region boss comes.
---@class GearRegion
---@field index integer The record's index (1 ..).
---@field id integer The region id the game uses.
---@field name string The region's name (may be empty).
---@field boss boolean The boss flag of the record.
---@field kills_needed integer The kills the region needs; 0 for a record the game did not set up. Writable (0 .. 1000000).
---@field kills_left integer The kills left before the boss. Writable (0 .. 1000000).
local GearRegion = {}

--- The region kill table. The game keeps its progress in memory only: it is not in savegames, and the game resets it when a region is left.
---@class GearRegions
local GearRegions = {}

--- The number of records (32; record 0 is unused), 0 if the table is not available.
---@return integer
function GearRegions:count() end

--- The record `index` (1 .. count - 1).
---@param index integer
---@return GearRegion?
function GearRegions:get(index) end

--- The region a sector lies in (a sector word, as `creature:sector()` gives). nil where the game has no region.
---@param sector integer
---@return GearRegion?
---@return integer? code The region byte the map files give the sector.
function GearRegions:of_sector(sector) end

--- The region a creature (for example the hero) is in.
---@param creature Creature
---@return GearRegion?
---@return integer? code
function GearRegions:of_creature(creature) end
