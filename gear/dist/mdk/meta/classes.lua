---@meta _
-- Sacred Gear: custom hero classes, ability copies and the models' motion slots.

-- ## Classes (gear.classes)
--: A custom class is a profile on top of one of the 8 hero bodies (creature types 1 to 9, `gear.world.CLASSES`): its skills, the arts
--: it can learn (from any class, and monster spells) and the clips it plays. These functions are the engine side of it; the mod
--: `custom-classes` builds the profiles on them. What the engine does, and which arts work on which body (the tiers):
--: [CustomClasses.md](CustomClasses.md). Events: `ability.route`, `motion.select`, `rune.usable`.

-- path gear.classes
--- Hero classes, combat arts and ability copies.
---@class GearClasses
local GearClasses = {}

--- One entry of `catalog()`.
---@class GearCatalogEntry
---@field id integer The ability id (combat arts from 1000, spells below; copies: 2000 to 4999 and 100 to 199).
---@field kind 'combat_art'|'spell'
---@field name string? The game's name text.
---@field name_text integer
---@field description_text integer
---@field target_mode integer How it is aimed (`gear.data.abilities`).
---@field homes integer[] The hero types whose action handlers know the art (empty for spells and monster-only actions).
---@field tiers table<integer, string> By hero type: `'home'`, `'A'` (spell: works as is), `'B'` (needs `ability.route` and a motion remap), `'C'` (class state: stays with its class) or `'D'` (no hero handler).
---@field runes integer[] Item types of the runes that teach it.
---@field alias integer? The base ability id, for a copy made by `copy_ability`.

--- Every ability with its home classes, its portability tier for each body and the runes that teach it.
---@return GearCatalogEntry[]
function GearClasses.catalog() end

--- The hero types (1 to 9) whose action handlers know a combat art.
---@param art integer
---@return integer[]
function GearClasses.homes(art) end

--- The tier of an ability for a body (`'home'`, `'A'`, `'B'`, `'C'`, `'D'`, see `GearCatalogEntry.tiers`).
---@param art integer
---@param body integer|string A hero type or its name (`'seraphim'`).
---@return string
function GearClasses.tier(art, body) end

--- The rune items that teach abilities: `{[ability id] = {item type, ...}}`.
---@return table<integer, integer[]>
function GearClasses.runes() end

--- Makes a copy of an ability: a new ability `alias` with its own record (a copy of `base`'s), which `values` change (the
--- field names of `gear.data.abilities`: `effect_base`, `value0_base`, `name_text` ...). A copy of an art has an id in 2000 to
--- 4999, a copy of a spell one in 100 to 199 (the engine refuses art ids above 5000). The copy runs the base ability's code
--- (its record keeps the base id, which the class handlers switch on); the hero's abilities, the art window, runes and
--- savegames use the copy's id. `gear.text.add` makes texts for `name_text` and `description_text`. The copies are removed
--- when the mod is switched off.
---@param alias integer
---@param base integer
---@param values? table<string, number>
---@return GearAbility? record
---@return string? error
function GearClasses.copy_ability(alias, base, values) end

--- Removes a copy this mod made.
---@param alias integer
---@return boolean removed
function GearClasses.remove_ability(alias) end

--- The raw Creature.pak hero record (0x56 bytes) of a body.
---@param body integer|string
---@return string? record
function GearClasses.hero_record(body) end

--- The skills of a body's hero record: two start skills and up to 16 available skills (ids 1 to 255).
---@class GearHeroSkills
---@field start integer[]
---@field available integer[]

---@param body integer|string
---@return GearHeroSkills?
function GearClasses.hero_skills(body) end

--- Replaces the skill lists in the game's hero record. It is the record of the body, so it applies to every hero of that body
--- (a mod switches it when the controlled hero changes) until `restore_hero_skills`.
---@param body integer|string
---@param start integer[] Two skill ids.
---@param available integer[] Up to 16.
---@return boolean? ok
---@return string? error
function GearClasses.set_hero_skills(body, start, available) end

--- Puts the original skill lists back.
function GearClasses.restore_hero_skills() end

--- Hero type from its name or number.
---@param body integer|string
---@return integer
function GearClasses.body_id(body) end

-- ## Models (gear.models)
--: Models.tmp gives every model 256 motion slots, each the index of a motion record (a clip) or 0 for none. The heroes share one
--: biped skeleton, so one body can play another's clip. Changes are for every creature of that model.

-- path gear.models
--- The models' motion slot tables.
---@class GearModels
local GearModels = {}

--- Whether the body's model has a clip for the slot (0 to 255); nil if the model was not found.
---@param body integer|string
---@param slot integer
---@return boolean?
function GearModels.has_motion(body, slot) end

--- The motion record the slot maps to (0: none), nil if the model was not found.
---@param body integer|string
---@param slot integer
---@return integer?
function GearModels.motion(body, slot) end

--- Maps the slot of the body's model to a motion record (what `motion()` of any body returns), undone by `restore`.
---@param body integer|string
---@param slot integer
---@param record integer
---@return boolean
function GearModels.set_motion(body, slot, record) end

--- Puts back the slots `set_motion` changed.
function GearModels.restore() end
