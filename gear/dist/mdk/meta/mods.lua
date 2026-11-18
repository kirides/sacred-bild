---@meta _
-- Sacred Gear: the mod manager: settings, key binds, the list of mods.

-- ## Settings, key binds and the mod manager
--: A mod declares its settings and key binds in `mod.lua` (`settings`, `keys`); the player changes them, switches the
--: mod on and off and rebinds keys in the **Mods** screen (F11, or the "Mods" button beside the escape menu), and
--: Sacred Gear keeps everything in `mods\gear_mods.json`. The mod reads its values with `gear.settings`, hears
--: changes as `settings.changed`, and gets declared keys through `gear.keys`. A mod switched off in the screen is
--: unloaded at once: it gets `unload`, then its handlers, timers, tasks, pipes, key binds, pickup rules and open
--: widgets are dropped (what it did to the world stays). Switching it on loads it again with a fresh environment
--: (`load`, and `session.start` when a game runs); mods it requires are switched on with it, and mods that require
--: it are off while it is.

---@alias GearSettingType
---| 'bool' # A checkbox: `true` or `false`.
---| 'number' # A slider from `min` to `max` in steps of `step` (defaults 0, 100, 1).
---| 'choice' # A dropdown: one of `choices` (a string or number each, or `{value, label}`).
---| 'text' # A line of text of at most `max_length` characters (default 120).
---| 'color' # A color: 0xAARRGGBB (set it from a number, `{r, g, b, a}` or `'#AARRGGBB'`).

--- One entry of a `choice` setting's `choices`.
---@class GearSettingChoice
---@field value string|number What the setting holds when this entry is picked.
---@field label? string What the screen shows (default: the value).

--- An entry of `settings` in `mod.lua`. The screen builds its page from these; `gear.settings.get(id)` returns the
--- player's value, else the mod's `config` or `config.lua` value for that id, else `default`.
---@class GearSettingDef
---@field id string Letters, digits, `_`, `-` and `.`; unique in the mod.
---@field type GearSettingType
---@field label? string Shown in the screen (default: the id).
---@field default? any The default value, of the setting's type (default: false, `min`, the first choice, `''`, opaque white).
---@field min? number For `number` (default 0).
---@field max? number For `number` (default 100).
---@field step? number For `number` (default 1).
---@field choices? (string|number|GearSettingChoice)[] For `choice`.
---@field max_length? integer For `text`.
---@field section? string A heading: settings in a row with the same section are shown under it.
---@field tooltip? string Shown when the cursor rests on the label.
---@field restart? boolean The mod only reads it at start: the screen says "after restart" (the mod still gets `settings.changed`).

--- An entry of `keys` in `mod.lua`: a key bind the player can change.
---@class GearKeyDef
---@field id string Letters, digits, `_`, `-` and `.`; unique in the mod.
---@field label? string Shown in the screen (default: the id).
---@field default? string A key with optional modifiers: `'f8'`, `'ctrl+m'`, `'ctrl+shift+k'`; `''` or nil: unbound.
---@field section? string A heading: keys in a row with the same section are shown under it.
---@field second? string The id of another key that is this one's second slot: the screen shows both on one row (the other key gets no row of its own).

-- path gear.settings
--- This mod's declared settings (`settings` in `mod.lua`) with the player's values. Values are checked against the
--- declaration: a number is clamped and put on the step grid, a choice must be one of the choices.
---@class GearSettings
local GearSettings = {}

--- The value of a setting. An undeclared UPPER_CASE name is a key of the game's `Settings.cfg` and gives that file's text
--- (see `game`), nil if it has no such key; any other undeclared name is an error.
---@param id string
---@return boolean|number|string
function GearSettings.get(id) end

--- The value of `key` in the game's `Settings.cfg` (`KEY : value` lines) as text, nil if the file has no such key.
--- Read each time, so it is what the file says now.
---@param key string
---@return string?
function GearSettings.game(key) end

--- Sets a value (kept in `gear_mods.json`); the mod's `settings.changed` handlers run if it changed. Returns true,
--- or nil and a message if the value does not fit the setting.
---@param id string
---@param value any
---@return boolean? ok
---@return string? error
function GearSettings.set(id, value) end

--- Whether the player has set this setting (else `get` returns the default).
---@param id string
---@return boolean
function GearSettings.is_set(id) end

--- Every setting's value, by id.
---@return table<string, boolean|number|string>
function GearSettings.all() end

--- Back to the default: one setting, or all of them.
---@param id? string
function GearSettings.reset(id) end

--- Once per mod and file: reads `name`, an old settings file of the mod's folder (JSON), and calls `fn` with the decoded
--- table, so the mod copies the values into its settings. Returns true if it ran, false if the migration was done before
--- or the file does not exist.
---@param name string
---@param fn fun(old: table)
---@return boolean ran
function GearSettings.migrate(name, fn) end

--- A registered key handler; pass it to `gear.keys.off`. Opaque.
---@class GearKeyHandle

-- path gear.keys
--- This mod's declared key binds (`keys` in `mod.lua`). A bind fires when its key is pressed with exactly its
--- modifiers held, once per press, in game and in automation; not while the Mods screen is open.
---@class GearKeys
local GearKeys = {}

--- Calls `fn` when the declared key `id` is pressed.
---@param id string
---@param fn fun()
---@return GearKeyHandle
function GearKeys.on(id, fn) end

--- Removes a handler.
---@param handle GearKeyHandle
function GearKeys.off(handle) end

--- The key bound to `id` now: the player's choice or the default, like `'ctrl+m'` (`''`: unbound).
---@param id string
---@return string
function GearKeys.binding(id) end

--- The key bound to `id` now as text for a window title or hint: `'Ctrl+G'`, `'F3'`, `''` if unbound. It follows the
--- player's rebinding, so a title like `'Loot log (' .. gear.keys.label('panel') .. ')'` stays right.
---@param id string
---@return string
function GearKeys.label(id) end

--- Rebinds a key from the mod itself (`''` unbinds, nil restores the default). Returns true, or nil and a message for
--- a text that is not a key.
---@param id string
---@param binding? string
---@return boolean? ok
---@return string? error
function GearKeys.set(id, binding) end

--- What `gear.mods` tells about an installed mod.
---@class GearModListing
---@field id string
---@field name string
---@field version string
---@field author string
---@field description string
---@field state 'on'|'off'|'blocked'|'pending'|'error'|'invalid' `blocked`: switched on but a mod it requires is off; `pending`: loads with the next tick; `error`: switched off after 10 errors or its `main.lua` failed.
---@field enabled boolean Whether the player has it switched on.
---@field loaded boolean
---@field builtin boolean The mod manager itself.
---@field reason? string Why it is blocked, failed or invalid.
---@field errors integer
---@field requires string[]
---@field has_settings boolean
---@field has_keys boolean
---@field has_page boolean
---@field dir string The folder under `mods`.

-- path gear.mods
--- The installed mods and the mod manager.
---@class GearMods
local GearMods = {}

--- Adds a page of this mod's own to its screen in the mod manager (a "More" tab after the generated settings):
--- `fn(x, y, w, h)` draws in that rectangle with the mod's own `gear.ui` and returns the height it used. nil removes it.
---@param fn? fun(x: number, y: number, w: number, h: number): number?
function GearMods.page(fn) end

--- Every installed mod, sorted by name.
---@return GearModListing[]
function GearMods.list() end

--- One installed mod, or nil.
---@param id string
---@return GearModListing?
function GearMods.get(id) end
