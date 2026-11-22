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
---@field id integer The ability id (combat arts from 1000, spells below; copies: 2000 to 4999).
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
--- field names of `gear.data.abilities`: `effect_base`, `value0_base`, `name_text` ...). Only combat arts can be copied (bases 1000 to 1999), under an id in
--- 2000 to 4999 (the engine refuses art ids above 5000; a copy of a spell was tried and is not run by the engine). The copy runs the base ability's code
--- (its record keeps the base id, which the class handlers switch on); the hero's abilities, the art window, runes and
--- savegames use the copy's id. `gear.text.add` makes texts for `name_text` and `description_text`. The copies are removed
--- when the mod is switched off.
---@param alias integer
---@param base integer
--- `values.spell` (a table of T2 field names) changes the spell record of an art whose numbers live in a spell (see `linked_spell`:
--- Poison Mist 1052 takes damage and duration from spell 12): the copy gets its own copy of that spell record, so the original art
--- and spell keep their numbers.
--- (The handlers push the spell id as a constant: Gear answers that lookup with the alias while the hero has the copy in flight. Tested in a game for Poison Mist; the other spell-linked arts use the same lookup but were not tested.)
---@param values? table<string, number|table<string, number>>
---@return GearAbility? record
---@return string? error
function GearClasses.copy_ability(alias, base, values) end

--- The id of the spell record the game's art `art` takes its numbers from (the engine's map 0x4D1E10: 1018 -> 8, 1019 -> 51, 1044 -> 47,
--- 1049 -> 45, 1050 -> 6, 1051 -> 48, 1052 -> 12, 1053 -> 46), nil for arts that use their own record.
---@param art integer
---@return integer?
function GearClasses.linked_spell(art) end

--- The values of the `target_mode` field of both ability tables (proved by the engine's launch check 0x51D840 and the loader's
--- ECF_* names): `free` 0, `hostile_creature` 1, `self_or_friend` 2, `ground_position` 3, `corpse` 4, `self_centered` 5.
---@type table<string, integer>
GearClasses.target_modes = {}

--- The bits of the `flags` field (T1 +0x74, T2 +0x90): `no_cast_mounted` 1, `mounted_needs_bow` 2, `two_handed_only` 4.
---@type table<string, integer>
GearClasses.ability_flags = {}

--- The bits of the `class_mask` field (T1 +0x70, T2 +0x8C): seraphim 1, gladiator 2, battle_mage 4, dark_elf 8, wood_elf 0x10,
--- vampiress 0x20, dwarf 0x400000, daemon 0x800000.
---@type table<string, integer>
GearClasses.ability_classes = {}

--- Removes a copy this mod made.
---@param alias integer
---@return boolean removed
function GearClasses.remove_ability(alias) end

--- The texture file name of an ability's icon (`GUI_MOVE_RUNDUM.TGA`), nil if it has none.
---@param id integer
---@return string?
function GearClasses.icon_name(id) end

--- The image id (for `gear.ui.image`) of an ability's icon, the game's own 64 x 64 texture, and the tint to draw it with (the
--- game's icons are grey discs: combat arts gold, spells pale blue); nil if it has none. A copy has its base's icon (give it
--- another with `gear.ui.texture_image(name)` and the record's icon string).
---@param id integer
---@return integer? image
---@return integer? color
function GearClasses.art_icon(id) end

--- One hero skill.
---@class GearSkill
---@field id integer
---@field name string
---@field description string?

--- A hero skill by its id (the u8 ids of the skill lists of `hero_skills`): name and description from the game's own texts, in the
--- game's language. The game has no skill icons. nil if there is no such skill.
---@param id integer
---@return GearSkill?
function GearClasses.skill(id) end

--- Every skill the game has, by id.
---@return GearSkill[]
function GearClasses.skills() end

---@class GearRuneOpts
---@field base_type? integer The item type whose record is copied (default: a rune of the copy's base art).
---@field name_text? integer The text id of the item's name (default: the copy's name).

--- Makes a rune item type for an ability copy: an item type of Gear's own (id `12800 + alias - 2000`, so copies 2000 to 4559) that is a copy
--- of the base art's rune, teaches the copy and carries its name. Items of that type work like the game's runes: they can be spawned
--- (`gear.world.spawn(type, x, y)`), picked up, traded, stored, and read (`creature:use_item`): the class rules of `rune.usable` apply and the art
--- is learned or levelled like with a native rune. Without Sacred Gear the type is an empty record: a savegame that holds such an item
--- loads it as an unknown item (see CustomClasses.md). Returns the item type, or nil and why.
---@param alias integer
---@param opts? GearRuneOpts
---@return integer? type
---@return string? error
function GearClasses.define_rune(alias, opts) end

--- The rune item type `define_rune` made for a copy.
---@param alias integer
---@return integer?
function GearClasses.rune_type_of(alias) end

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
