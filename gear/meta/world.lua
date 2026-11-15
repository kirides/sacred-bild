---@meta _
-- Sacred Gear: the world.

-- ## World (gear.world)

--- A hero class: the hero's type (1..9) and the bit an item's class mask has for it.
---@class GearClassInfo
---@field id integer
---@field name 'seraphim'|'gladiator'|'battlemage'|'darkelf'|'woodelf'|'vampiress'|'dwarf'|'daemon'
---@field bit integer

---@alias GearQuality 'normal'|'magic'|'rare'|'set'|'unique'
---@alias GearItemCategory 'weapon'|'armor'|'shield'|'jewelry'|'potion'|'rune'|'scroll'|'special'|'gold'|'other'
---@alias GearPotionKind 'health'|'mana'|'stamina'|'colored'|'other'
---@alias GearPotionColor 'black'|'blue'|'green'|'red'|'yellow'
---@alias GearClassName 'seraphim'|'gladiator'|'battlemage'|'darkelf'|'woodelf'|'vampiress'|'dwarf'|'daemon'

-- path gear.world
--- Positions are world units (`TILE`, about 53.7, is one tile; the screen shows about 534 x 400 around the hero) and a
--- layer (0 outdoors; dungeons have their own). The functions that change the world work only in single player
--- (`can_change()`); in a network game they return nil and a message.
---@class GearWorld
---@field TILE number World units per tile.
---@field CLASSES GearClassInfo[] The hero classes.
---@field QUALITIES GearQuality[] Item qualities in ascending order; `quality_rank` is the index.
local GearWorld = {}

--- The local hero, or nil.
---@return Creature?
function GearWorld.hero() end

--- The object with `id` (as a creature handle), or nil if there is none.
---@param id integer
---@return Creature?
function GearWorld.object(id) end

--- Whether the world may be changed now (a single player game runs).
---@return boolean
function GearWorld.can_change() end

--- Creatures within `radius` world units of (x, y), nearest first.
---@param x number
---@param y number
---@param radius number
---@param opts? GearNearOpts
---@return Creature[]
function GearWorld.creatures_near(x, y, radius, opts) end

--- Items lying in the world within `radius` of (x, y), nearest first.
---@param x number
---@param y number
---@param radius number
---@param opts? GearNearOpts
---@return GearItem[]
function GearWorld.items_near(x, y, radius, opts) end

--- A random point between `min` and `max` world units from (x, y).
---@param x number
---@param y number
---@param min? number Default 0.
---@param max? number Default `min`.
---@return integer x
---@return integer y
function GearWorld.around(x, y, min, max) end

--- Creates an object of `type` (a type id or an object type name) at (x, y), as the game's own scripts do. Single
--- player only.
---@param type integer|string
---@param x number
---@param y number
---@param opts? GearSpawnOpts
---@return Creature? creature
---@return string? error
function GearWorld.spawn(type, x, y, opts) end

--- An item object as a table, or nil.
---@param id integer
---@return GearItem?
function GearWorld.item(id) end

--- Whether `class` can use the item.
---@param item GearItem
---@param class GearClassName
---@return boolean
function GearWorld.usable_by(item, class) end

--- Sets this mod's pickup rules, replacing its previous ones (`nil`: none). The local hero does not pick up the `block`
--- items (a click on them does nothing, the automatic collecting passes them by), and the ground labels of the `dim`
--- items are drawn with their alpha scaled to `dim_alpha`. The rules of all mods are merged after each tick. They hold
--- in single player and for the host's own hero. Item ids are only valid while the item exists.
---@param rules? GearPickupRules
function GearWorld.pickup_rules(rules) end

--- The number of pickups refused so far.
---@return integer
function GearWorld.pickup_refused() end

---@class GearNearOpts
---@field layer? integer The layer (default 0).
---@field limit? integer At most this many (default 64).

---@class GearSpawnOpts
---@field layer? integer Default: the hero's layer.
---@field label? any Kept for the creature this session (`creature:label()`).

---@class GearPickupRules
---@field block? integer[] Item ids the local hero does not pick up.
---@field dim? integer[] Item ids whose ground labels are dimmed.
---@field dim_alpha? integer Alpha 0..255 for dimmed labels (default 96).

--- What `creature:info()` returns.
---@class GearObjectInfo
---@field id integer
---@field type integer The object type id.
---@field flags integer
---@field x integer
---@field y integer
---@field layer integer
---@field creature boolean Whether the object is a creature.
---@field hp? integer Creatures only.
---@field max_hp? integer Creatures only.
---@field gold? integer Creatures only (heroes).
---@field exp? integer Creatures only.
---@field level? integer Creatures only.

--- An item object, from `gear.world.item` or `items_near`.
---@class GearItem
---@field id integer
---@field type integer The object type id.
---@field type_name string? The object type's name.
---@field kind integer The type record's kind.
---@field category GearItemCategory
---@field rarity integer 0..15.
---@field quality GearQuality 'normal' 0-4, 'magic' 5-11, 'rare' 12-14, 'unique' 15, 'set' for set items.
---@field quality_rank integer 1..5 in `gear.world.QUALITIES` order.
---@field set boolean A set item.
---@field classes GearClassName[] The hero classes that can use it (empty: every class).
---@field class_mask integer The raw class mask.
---@field gold boolean Gold.
---@field quest boolean A quest item.
---@field potion? GearPotionKind Potions only.
---@field potion_color? GearPotionColor Colored potions only.
---@field on_ground boolean Lying in the world.
---@field flags integer Raw object flags.
---@field type_flags integer Raw type flags.
---@field x integer
---@field y integer
---@field layer integer

-- path creature
--- A creature object: a small handle that reads the game each time (after the object is gone the reads give nil).
--- Handles of the same object are equal (`==`).
---@class Creature
---@field id integer The object id.
local Creature = {}

---@return boolean
function Creature:exists() end

--- All fields, or nil.
---@return GearObjectInfo?
function Creature:info() end

---@return boolean?
function Creature:is_creature() end

--- The object type id.
---@return integer?
function Creature:type() end

---@return string?
function Creature:type_name() end

--- World position and layer.
---@return integer? x
---@return integer? y
---@return integer? layer
function Creature:position() end

---@return integer?
function Creature:hp() end

---@return integer?
function Creature:max_hp() end

--- True when it has no hit points left or is gone.
---@return boolean
function Creature:is_dead() end

---@return integer?
function Creature:level() end

---@return integer?
function Creature:experience() end

---@return integer?
function Creature:gold() end

--- Distance in world units to another creature or to (x, y).
---@param other Creature|number
---@param y? number
---@return number?
function Creature:distance(other, y) end

--- The label `gear.world.spawn` kept for the creature this session.
---@return string?
function Creature:label() end

--- The hero's class name and id; nil for other creatures.
---@return GearClassName? name
---@return integer? id
function Creature:class() end

--- Sets the hit points (1..max: mods do not kill through it). Single player only.
---@param hp number
---@return boolean? ok
---@return string? error
function Creature:set_hp(hp) end

--- Sets the gold (heroes). Single player only.
---@param gold number
---@return boolean? ok
---@return string? error
function Creature:set_gold(gold) end

--- Adds gold, negative amounts take it away (heroes). Single player only.
---@param amount number
---@return boolean? ok
---@return string? error
function Creature:add_gold(amount) end

--- Moves the creature. Single player only.
---@param x number
---@param y number
---@param layer? integer Default: the current layer.
---@return boolean? ok
---@return string? error
function Creature:teleport(x, y, layer) end
