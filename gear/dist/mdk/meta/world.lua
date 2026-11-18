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
---@field QUALITY_COLORS table<GearQuality, integer> The game's item label colours (ARGB) by quality: white, blue, yellow, gold, green for set items.
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

--- The colour of a quality (ARGB, `QUALITY_COLORS`): a quality name, or a rarity 0..15 with `set` for set items.
---@param quality GearQuality|integer
---@param set? boolean
---@return integer
function GearWorld.quality_color(quality, set) end

--- The colour the game gives an item's name and ground label (ARGB): as `quality_color`, but items for one class take
--- that class's colour unless the game's UNIQUE_COLOR setting is on. Nil when the item is gone.
---@param item GearItem|integer An item or its id.
---@return integer?
function GearWorld.item_color(item) end

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
---@field sector integer The sector word of its position: the slot of the game's sector table (not a coordinate), 0 when none.
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

--- The sector the object's position is filed under: a slot of the game's sector table (the number a game server's
--- scripts see as `player.sector`; ids are not coordinates), 0 when there is none, nil when the object is gone.
---@return integer?
function Creature:sector() end

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

--- A hero's name as the game shows it; nil for other creatures (and for a hero the game has no name for).
---@return string?
function Creature:name() end

--- Sets the hit points (1..max: mods do not kill through it). Single player only.
---@param hp number
---@return boolean? ok
---@return string? error
function Creature:set_hp(hp) end

--- Sets the local hero's gold (0 to 2,147,483,646), also in LAN and Open Net games: through the engine's own gold
--- setter, as a merchant sale does, so the hero's next update tells the server and the other players. Other heroes
--- are refused (only their own computer may change them).
---@param gold number
---@return boolean? ok
---@return string? error
function Creature:set_gold(gold) end

--- Adds gold to the local hero, negative amounts take it away (not below 0); also in network games, as `set_gold`.
---@param amount number
---@return boolean? ok
---@return string? error
function Creature:add_gold(amount) end

--- Removes the creature from the world at once, through the engine's own object destroy: no corpse, no loot, no experience. The handle is dead afterwards (`exists()` is false). Heroes are refused. Single player only.
---@return boolean? ok
---@return string? error
function Creature:remove() end

--- Moves the creature. Single player only.
---@param x number
---@param y number
---@param layer? integer Default: the current layer.
---@return boolean? ok
---@return string? error
function Creature:teleport(x, y, layer) end

--- Whether the creature is a hero (a player's or a companion).
---@return boolean
function Creature:is_hero() end

--- Whether `other` is alive and hostile to this creature (what the cursor's attack symbol shows). Nil when one of
--- them is not a creature.
---@param other Creature|integer
---@return boolean?
function Creature:is_enemy(other) end

--- The image of the creature's portrait as the party window draws it (an id for `gear.ui.image`), nil if it has none.
---@return integer?
function Creature:portrait() end

--- A hero's combat arts and spells (see `gear.party`), nil for other creatures.
---@return GearHeroAbility[]?
function Creature:abilities() end

--- One of the hero's combat arts or spells, nil if it does not know it.
---@param id integer
---@return GearHeroAbility?
function Creature:ability(id) end

--- What a hero wears: its equipment slots that hold an item (the game's slot numbers 0 to 18), nil for other
--- creatures.
---@return {slot: integer, item: GearItem}[]?
function Creature:equipment() end

--- A hero's combos (the combo master's slots that hold arts), nil for other creatures. Use one with
--- `order_ability(combo.id, target)`.
---@return GearHeroCombo[]?
function Creature:combos() end

--- Writes a combo slot of a party hero as the combo master does: up to four combat arts it knows, at the levels it has
--- them (an empty list clears the slot). Single player.
---@param slot integer 0 to 3.
---@param arts integer[]
---@return boolean? ok
---@return string? error
function Creature:set_combo(slot, arts) end

--- What a party hero does now, and the game's raw action and movement states.
---@return GearHeroAction? action
---@return integer? action_state
---@return integer? movement_state
function Creature:action() end

--- Orders a party hero (`gear.party`) to walk to (x, y), as a click on the ground does for the controlled one.
---@param x number
---@param y number
---@return boolean? ok
---@return string? error
function Creature:order_move(x, y) end

--- Orders a party hero to stop.
---@return boolean? ok
---@return string? error
function Creature:order_stop() end

--- Orders a party hero to attack `target` with its weapon (it walks there first), as a click on an enemy does.
---@param target Creature|integer
---@return boolean? ok
---@return string? error
function Creature:order_attack(target) end

--- Orders a party hero to use a combat art or spell it knows, or one of its combos (`combos()`, ids 0x200 + slot), on
--- a creature (`target`) or a spot (`x`, `y`), as the skill slot does for the controlled one. The game refuses it while
--- the ability regenerates (`ability(id).ready`, `combo.ready`).
---@param ability integer
---@param target Creature|integer|number A creature, or the x of a spot.
---@param y? number The y of a spot.
---@return boolean? ok
---@return string? error
function Creature:order_ability(ability, target, y) end

--- Sets the name the game shows for a party hero (1 to 30 characters, UTF-8). The game keeps a companion's name only
--- until the next savegame load: a mod sets it again (the player's own hero keeps the name the savegame has).
---@param name string
---@return boolean? ok
---@return string? error
function Creature:set_name(name) end

--- Lets a party hero drink a potion from its own backpack, as the potion keys do for the controlled hero (the game
--- picks the potion). Fails if it carries none.
---@param kind? 'health'|integer `'health'` (default, what Space drinks) or the game's potion kind number.
---@return boolean? ok
---@return string? error
function Creature:drink_potion(kind) end

--- Brings a fallen party hero back (`gear.party`). The engine removes a fallen companion from the world (with what
--- it wears and carries) about 25 seconds after it fell, as a network game removes a dead player's hero: revive it
--- before that.
---@param hp? number Hit points (1 to the maximum; default a quarter of the maximum).
---@return boolean? ok
---@return string? error
function Creature:revive(hp) end

--- Gives a party hero experience through the game's own gain (it levels up with skill points). Not shared.
---@param amount number
---@return boolean? ok
---@return string? error
function Creature:add_experience(amount) end
