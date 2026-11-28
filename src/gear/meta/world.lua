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

--- The bits of the object flag word (`creature:info().flags`, the object's +0x14).
---@class GearObjectFlags
---@field NOT_IN_WORLD integer 0x2: carried or being picked up (an item), not lying in the world.
---@field REMOVED integer 0x10: freed or parked; the object is gone.
---@field DIALOGUE_ICON integer 0x80000: the "!?" icon over the creature's head. Every dialogue assignment sets it; the game clears it when the dialogue goes. The speech-bubble cursor does not read it, it reads the dialogue id.
---@field THROWN integer 0x100000: in a throw's flight.
---@field MADE_BY_ENTER integer 0x80000000: made by a sector's Enter function (the fills' creatures among them).

--- The bits of the behavior word (the second value of `creature:allegiance()`, +0x1F4): what a creature does by itself. Several are the modes the scripts' creature maker (`CreateNPC`) gives by its tags.
---@class GearBehaviorFlags
---@field AGGRESSIVE integer 0x1: hostile and awake, attacks.
---@field FOLLOWER integer 0x4: follows a partner (the scripts' companions).
---@field TOWNSPERSON integer 0x8: the townsfolk's mode; 0x8000 runs as this one.
---@field ANIMAL_10 integer 0x10: an animal mode (moves only while standing).
---@field ANIMAL_20 integer 0x20: an animal mode (the loose farm animals').
---@field ANIMAL_40 integer 0x40: an animal mode.
---@field ANIMAL_80 integer 0x80: an animal mode.
---@field HIRED integer 0x100: a hired follower at its master's side.
---@field TOWNSPERSON_400 integer 0x400: the townsfolk's second mode.
---@field STANDS_800 integer 0x800: stands.
---@field STANDS_1000 integer 0x1000: stands.
---@field GUARD integer 0x4000: the guards' mode.
---@field TOWNSPERSON_8000 integer 0x8000: runs as TOWNSPERSON.
---@field SLIDE integer 0x20000: a slide.
---@field NO_TALK integer 0x40000: gets no generic dialogue and no speech-bubble cursor.
---@field BOSS integer 0x80000: a boss; its ring is drawn under it.
---@field FLYING integer 0x200000: flies.
---@field SIGHT integer 0x400000: runs its sight function once the hero comes in view.
---@field JUGGLER integer 0x10000000: the jugglers' mode.
---@field EFFECT_32_MARK integer 0x40000000: set while effect 32 holds; the creature render draws an extra marker over it.
---@field TOWNSFOLK_MASK integer 0x1006DCF8: the bits that let a creature talk by the generic dialogues, show a doing's icon (Alt) and take the AI's behaviours.

--- The bits of the creature state word (+0x200).
---@class GearStateFlags
---@field IN_TALK integer 0x10: in a talk; no speech-bubble cursor while it is set.
---@field PLAYER_HERO integer 0x40: a player's hero (the game's own reactions are off).
---@field FIGHT_STANCE integer 0x8000: the fight stance, enemies about (what `creature:in_combat()` reads).
---@field HELD integer 0x40000: held by a stun, knock-down or knock-back.
---@field SMITH integer 0x1000: a smith (hammer cursor, smith window).
---@field MERCHANT integer 0x2000: a merchant (bag cursor, trade window).
---@field COMBAT_MASTER integer 0x4000: a combat art master.
---@field HORSE_TRADER integer 0x4000000: the horse trader.

---@class GearCreatureFlags
---@field object GearObjectFlags
---@field behavior GearBehaviorFlags
---@field state GearStateFlags

---@class GearWorldEnums
---@field ai_state table<'IDLE'|'GO_TO'|'BEHAVIOR'|'MELEE'|'CAST'|'RANGED'|'DEAD'|'TALK'|'WEAPON_ART'|'PICK_UP', integer> A creature's AI state (+0xFC): the switch of the creatures' update.
---@field action table<'STAND'|'MOVE'|'MELEE_STRIKE'|'CAST'|'SHOOT'|'DYING'|'TALK'|'GLADIATOR'|'SERAPHIM'|'DARK_ELF'|'WOOD_ELF'|'BATTLE_MAGE'|'VAMPIRESS'|'DWARF'|'DAEMON', integer> A creature's action (+0x150, what `creature:action()` reports): 8 to 15 are the hero classes' own arts.
---@field creature_class table<'HERO'|'MONSTER'|'NPC'|'HORSE'|'UNDEAD'|'ANIMAL'|'MERCENARY'|'GOBLIN'|'DEMON'|'DRAGON'|'ENERGY'|'ELF'|'ALL_ENEMIES'|'HUMAN'|'DRYAD', integer> The class byte of a creature type. 1 to 6 are tested by the game's own predicates; 7 to 15 take their names from the open reimplementations.
---@field creature_type_flags table<'FLIES'|'GHOST', integer> The flag word of a creature type.
---@field item_kind table<string, integer> The kind of an item or object type record (`GearItem.kind`): WORLD_OBJECT, CREATURE, CONTAINER, WEAPON, CHEST_ARMOR, RING, POTION, DOOR, EFFECT, SHIELD, KEY, QUEST_ITEM, BOOK, HELMET, FOOT_ARMOR, BELT, AMULET, SHOULDER_ARMOR, ARM_ARMOR, LEG_ARMOR, GLOVES, WINGS, SMITHING_ACTION, PROJECTILE, RUNE, HORSE_EQUIPMENT, DWARF_CANNON. WEAPON, CHEST_ARMOR, RING, POTION, SHIELD, HELMET, FOOT_ARMOR, BELT and AMULET are the ones `gear.shop.CATEGORY` uses; the others rest on the reimplementation's names.
---@field equipment_slot table<'HEAD'|'BODY'|'BELT'|'FEET'|'GLOVES'|'ARMS'|'AMULET_1'|'AMULET_2'|'RING_1'|'RING_2'|'RING_3'|'RING_4'|'WEAPON'|'SHIELD'|'CANNON'|'SHOULDER'|'LEGS'|'WINGS'|'MOUNT', integer> The creature's equipment slots (0 to 18), as `creature:carry(slot, item)` takes them.

-- path gear.world
--- Positions are world units (`TILE`, about 53.7, is one tile; the screen shows about 534 x 400 around the hero) and a
--- layer (0 outdoors; dungeons have their own). The functions that change the world work only in single player
--- (`can_change()`); in a network game they return nil and a message.
---@class GearWorld
---@field TILE number World units per tile.
---@field CLASSES GearClassInfo[] The hero classes.
---@field FLAGS GearCreatureFlags The named bits of a creature's flag words (object, behavior, state), read from the game's own tests of them. `gear.world.has_flag(word, mask)` tests one.
---@field ENUMS GearWorldEnums Named numbers of the engine: AI states, actions, creature classes, creature type flags, item kinds and equipment slots, each a table of name to number.
---@field QUALITIES GearQuality[] Item qualities in ascending order; `quality_rank` is the index.
---@field QUALITY_COLORS table<GearQuality, integer> The game's item label colours (ARGB) by quality: white, blue, yellow, gold, green for set items.
local GearWorld = {}

--- True where `word` has any bit of `mask`, e.g. `gear.world.has_flag(creature:info().flags, gear.world.FLAGS.object.DIALOGUE_ICON)`.
---@param word integer?
---@param mask integer
---@return boolean
function GearWorld.has_flag(word, mask) end

--- The local hero, or nil.
---@return Creature?
function GearWorld.hero() end

--- The object with `id` (as a creature handle), or nil if there is none.
---@param id integer
---@return Creature?
function GearWorld.object(id) end

--- The game's clock, the one the day and night cycle runs on: minutes since midnight (0 to 1439), the hour and the minute. nil if
--- the game's clock was not found (no game running).
---@return integer? minutes
---@return integer? hour
---@return integer? minute
function GearWorld.clock() end

--- Whether the world may be changed now (a single player game runs).
---@return boolean
function GearWorld.can_change() end

--- Opens the game's list of the portals the local hero knows (the "Transferportal" window), as a portal does: the hero can move to any of
--- its open portals. `sub` is the script command's sub-command, 5 or 6 (both show the list); by default the game's own kind word picks it, as
--- ReBorn's teleportation potion does (6 when the word is 1, which it is in the plain game, else 5). Nothing is charged.
---@param sub integer? 5, 6 or nil (the game's choice).
---@return boolean? ok
---@return string? error
function GearWorld.open_teleporter(sub) end

--- Shows a line of the game's text `text_id` in the HUD, as the game's own refusals ("Enemy is near!") do. `color` is 0xAARRGGBB (the
--- game's refusals use 0xFFFF0000, red; 0 or nil leaves it to the game).
---@param text_id integer A text id of the game's texts (`gear.text.get`), below 0x80000000.
---@param color integer?
---@return boolean? ok
---@return string? error
function GearWorld.show_text(text_id, color) end

--- The portals the local hero knows (one bit per portal, as the script command Teleporter sets them). With `mask` they are set (single
--- player; the other players are not told).
---@param mask integer?
---@return integer? mask
---@return string? error
function GearWorld.portals(mask) end

--- Creatures within `radius` world units of (x, y), nearest first.
---@param x number
---@param y number
---@param radius number
---@param opts? GearNearOpts
---@return Creature[]
function GearWorld.creatures_near(x, y, radius, opts) end

--- What the mouse cursor is over in the world (not over a window): the object it picks as the game's own cursor does (the hero
--- itself is skipped when `exclude` is given), and the world point and layer under it, in the units of `creature:position()`.
---@class GearCursor
---@field object Creature? The creature (or other object) under the cursor.
---@field x integer
---@field y integer
---@field layer integer
---@field pixel_x integer The cursor in the game window's pixels.
---@field pixel_y integer

---@param exclude? Creature|integer An object the pick skips (the hero).
---@return GearCursor?
function GearWorld.cursor(exclude) end

--- Items lying in the world within `radius` of (x, y), nearest first.
---@param x number
---@param y number
---@param radius number
---@param opts? GearNearOpts
---@return GearItem[]
function GearWorld.items_near(x, y, radius, opts) end

--- The creatures that have modifiers now (`creature:set_modifiers`), by id.
---@return Creature[]
function GearWorld.modified() end

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
---@field like? Creature|integer A creature whose allegiance (`creature:copy_allegiance`) and level, stats and health share (`creature:copy_stats`) the new one takes; if that fails the new creature is removed and spawn fails. A plain create has the faction of its type, which can differ from a creature the game placed (a soldier hostile to the hero by its spawn, neutral by its type).
---@field label? any Kept for the creature this session (`creature:label()`).
---@field modifiers? GearModifiers Boss modifiers (`creature:set_modifiers`) set on the new creature; if they are refused the creature is removed again and spawn fails.

--- Boss modifiers of one creature (`creature:set_modifiers`). A field left out does nothing.
--- The health is written into the creature (maximum and current value); the rest is applied by Gear's `damage` hook
--- on every hit the engine resolves, so it holds for melee, arrows and spells alike as far as they reach the hook.
--- Modifiers exist for this session only: they are forgotten when a savegame is loaded (object ids then mean other
--- creatures). A savegame written while a boss exists keeps it as a plain strong creature (its boosted maximum and current
--- health); to keep the boss, store its id, type and `base_hp` in `gear.store.game` and call
--- `set_modifiers{health = ..., base_hp = ...}` after the load (the world-events mod does). Not available: size, movement
--- speed and resistances other than `taken`. Test: `src/gear/examples/bosstest`.
---@class GearModifiers
---@field health? number Maximum health as a multiple of the creature's own (0.01 to 100000); the creature starts at full health.
---@field max_hp? integer Maximum health as an absolute number (wins over `health`).
---@field damage? number Multiplier of the damage the creature deals (0 to 1000; the engine keeps 16 bit: at most 65534 per hit).
---@field taken? number Multiplier of the damage the creature takes (0 to 1000; 0.5 is a deflecting armour, 0 immunity).
---@field deflect? number Chance 0 to 1 that a hit does no damage at all.
---@field reflect? number Fraction (0 to 10) of the damage the creature takes that the attacker takes back. The attacker keeps at least 1 hit point (reflection never kills); nothing is reflected onto the creature itself.
---@field regen? number Health regained per second as a fraction of the maximum (0 to 1, e.g. 0.01).
---@field base_hp? integer The creature's own maximum health, when `health` is applied to a creature that already holds the boosted maximum: a creature loaded from a savegame that was written while it was a boss. Without it the creature's current maximum counts as its own.

---@class GearModifierStats
---@field base_max_hp integer The creature's own maximum health before the modifiers.
---@field hits integer Hits the creature took.
---@field deflected integer Of them, the ones it deflected.
---@field damage integer Damage it took (after `taken`).
---@field reflected integer Damage reflected onto attackers.
---@field dealt integer Hits it dealt with `damage` applied.
---@field max_hp_resets integer How often the engine put the creature's own maximum health back and Gear wrote the boss maximum again.

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
---@field in_combat? boolean Creatures only: the engine's own "enemies about" state (see `Creature:in_combat`).
---@field casting? integer Creatures only: the art or spell in flight, 0 for none (see `Creature:casting`).

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

--- The creature's name for the player: its own name text (`creature:set_name_text`) if it has one, else the localized name
--- of its type (`gear.data.object_types:display_name`), the internal type name when the game has no text for it. Use it for
--- anything the player reads; `type_name` stays the identifier to match on.
---@return string?
function Creature:display_name() end

--- The text id of the creature's own name (the creature's `+0x3C`), 0 if it shows its type's name, nil if there is no such creature.
---@return integer?
function Creature:name_text() end

--- Gives the creature (not a hero) a name of its own: the game shows this text over it, in its hover label and its lists, instead
--- of its type's name; 0 gives the type's name back. The field is saved with the savegame, so use an id of `gear.text.key`, which
--- is the same in every session. Single player.
---@param id integer
---@return boolean? ok
---@return string? error
function Creature:set_name_text(id) end

--- Draws the creature's model with a colour multiplied over it. `spec`: a colour (0xAARRGGBB; a number below 0x1000000 is taken as
--- 0xRRGGBB, opaque) for the whole model, or `{all = colour, parts = {[1] = colour, ...}}` for single meshes in the order the game
--- draws them (a character is made of a few meshes, for example the body and the cloth); a part's colour wins over `all`. nil clears it.
--- Textures are multiplied, so a light texture takes the colour best. Needs SacredBild; single player; the tints end with the savegame
--- (ids then mean other creatures), so a mod sets them again after a load.
---@param spec integer|{all: integer?, parts: table<integer, integer>?}?
---@return boolean? ok
---@return string? error
function Creature:set_tint(spec) end

--- Draws the creature's model with other textures. `spec`: an image id for the whole model, or `{all = id, parts = {[n] = id}}` for single
--- meshes in the order the game draws them (as `set_tint`'s parts). An id is `gear.ui.texture_image(name)` (a texture of the game's
--- Texture.pak) or the `picture` of a `gear.ui.texture.load(path)` handle (an image file of the mod: PNG, TGA, JPG or DDS, both sides a
--- power of two; keep the handle as long as the creature uses it). The texture replaces the mesh's own one, so it needs the same texture
--- layout (the skin of a model of the same family); opaque images only. nil clears it. Needs SacredBild; single player; the textures end
--- with the savegame, so a mod sets them again after a load.
---@param spec integer|{all: integer?, parts: table<integer, integer>?}?
---@return boolean? ok
---@return string? error
function Creature:set_texture(spec) end

--- Puts an item object (made with `gear.world.spawn`, lying in the world) into equipment slot 0 to 18 of a creature that is no hero: the item leaves the world and
--- the creature carries it there, as the creatures the game places do. A weapon in slot 12 is wielded (the model shows it), a shield in 13. Single player.
---@param slot integer
---@param item GearItem|integer
---@return boolean? ok
---@return string? error
function Creature:carry(slot, item) end

--- The creature's faction (0 to 15, the class the game's hostility table is indexed by) and whether its behavior flags say aggressive.
---@return integer? faction
---@return boolean? aggressive
function Creature:allegiance() end

--- Sets the faction and/or the aggression of a creature (a field left out stays). Not for heroes. Single player.
---@param spec {faction: integer?, aggressive: boolean?}
---@return boolean? ok
---@return string? error
function Creature:set_allegiance(spec) end

--- The `gear.text.key` key of the creature's own name text, if a mod registered it this session.
---@return string?
function Creature:name_key() end

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

--- Whether the engine's own "enemies about" state is set for the creature (bit 0x8000 of its state word, which the engine's creature code
--- sets while enemies are near and clears a while after): the state that stops ReBorn's teleportation potion.
---@return boolean
function Creature:in_combat() end

--- The id of the art or spell the creature has in flight (a creature's word at +0x120; kept after the cast until the next one starts, so it
--- tells what it cast last, not that it is casting now), nil when none.
---@return integer?
function Creature:casting() end

--- Sends the creature the message a drunk potion sends its drinker, so a mod can give a potion its own numbers (the potion handler's effects:
--- 6, 7 and 9 refill health or mana, 8 removes poison by `value` percent, 10 the concentration potions' combo refill).
---@param effect integer
---@param value integer
---@return boolean? ok
---@return string? error
function Creature:apply_effect(effect, value) end

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

--- Gives this creature the level of `source` through the game's own level function (the one its sector fill uses): the experience
--- the level starts at, the derived stats and the maximum health come from the creature's type and that level; the current health
--- keeps the donor's share of the maximum. A creature made by `gear.world.spawn` is level 1 with 100 health whatever its type.
--- Single player; not for heroes.
---@param source Creature|integer
---@return boolean? ok
---@return string? error
function Creature:copy_stats(source) end

--- Gives this creature the allegiance of `source`: its faction (the class the game's hostility table is indexed by) and the aggression
--- flag. A creature made by `gear.world.spawn` has the faction of its type; one the game placed (a sector fill, a script) can have
--- another, so a copy of a hostile soldier may be neutral until this is called. Single player.
---@param source Creature|integer
---@return boolean? ok
---@return string? error
function Creature:copy_allegiance(source) end

--- Sets the level of a creature (not a hero) through the game's own level function, the one its sector fill uses: the experience the level starts at, the derived stats and the maximum health come from the creature's type and that level (a creature made by `gear.world.spawn` is level 1). It keeps its share of the health. Level 1 to 250. Single player.
---@param level integer
---@return boolean? ok
---@return string? error
function Creature:set_level(level) end

---@class GearPetOpts
---@field scale? number The model is drawn this many times as large (0.1 to 4, default 1), about the creature's own origin.
---@field untargetable? boolean The creature is left out of the world pick: no click, hover cursor or controller aim reaches it (default false).

--- Makes the creature a pet: drawn larger and/or not targetable (SacredBild; the hitbox and the creature's own logic are unchanged). `nil` or `{}` clears it. Forgotten when the game ends or another savegame loads (object ids then mean other creatures). Single player.
---@param opts GearPetOpts?
---@return boolean? ok
---@return string? error
function Creature:set_pet(opts) end

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

--- Gives the creature boss modifiers (`GearModifiers`: health, damage dealt and taken, deflection, reflection,
--- regeneration), replacing its previous ones; an empty table or nil clears them (the health goes back to the
--- creature's own maximum). Single player only, creatures that are not heroes. Gear reads and writes the maximum
--- health at +0x4D4 and the current at +0x4D8; the effect on hits needs the `damage` hook (installed on demand).
---@param mods? GearModifiers
---@return boolean? ok
---@return string? error
function Creature:set_modifiers(mods) end

--- The modifiers set on the creature (a copy), nil if it has none (or is gone).
---@return GearModifiers?
function Creature:modifiers() end

--- What the modifiers did so far, nil if the creature has none.
---@return GearModifierStats?
function Creature:modifier_stats() end

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

--- A hero's weapon slots (the taskbar's keys 1-5): which one is active and the weapons in each (1-based; `main`, `off` the
--- main and off-hand items, nil when empty). Nil for other creatures.
---@return {active: integer, slots: {main: GearItem?, off: GearItem?}[]}?
function Creature:weapon_slots() end

--- Takes up the weapons of weapon slot `n` (1-based), as the keys 1-5 do for the hero you play: the old ones go back to
--- their slot. For party heroes (single player).
---@param n integer
---@return boolean? ok
---@return string? error
function Creature:use_weapon_slot(n) end

--- A hero's experience, the experience its level started at and the experience the next level starts at (the game's
--- own levels), for a gauge: `(now - from) / (to - from)`. Nil for other creatures.
---@return integer? experience
---@return integer? from
---@return integer? to
function Creature:experience_range() end

--- What any creature carries in its equipment slots (0 to 18): monsters, mercenaries and heroes alike. A monster holds the gear that
--- the Disarming skill takes from it there. Nil if the creature is gone.
---@return {slot: integer, item: GearItem}[]?
function Creature:carried() end

--- The equipment slot (0..18) an item goes to for this hero, by the engine's own rule: for a group of slots the first one
--- (amulets 6, 7; rings 8 to 11; weapons 12; shields and off-hand items 13), whatever the hero wears. Nil when the hero cannot
--- wear it or it is no equipment.
---@param item GearItem|integer An item or an item type id.
---@return integer?
function Creature:equip_slot(item) end

--- A hero's combos (the combo master's slots that hold arts), nil for other creatures. Use one with
--- `order_ability(combo.id, target)`.
---@return GearHeroCombo[]?
function Creature:combos() end

--- Sets how far a party hero's combo has regenerated, in seconds of the combo's `total` (`GearHeroCombo`): it is ready when `charge >= total`.
--- The value is not clamped above (the engine accepts a charge over the total); a negative value becomes 0. Single player and the local
--- player's own heroes.
---@param slot integer 0 to 3.
---@param charge number
---@return boolean? ok
---@return string? error
function Creature:set_combo_charge(slot, charge) end

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

--- Orders a party hero (`gear.party`) to walk to (x, y), as a click on the ground does for the controlled one. In single player (`gear.world.can_change()`) this also works on any other creature, a summoned wolf for example (a creature of the game's own AI may walk back to what it was doing).
---@param x number
---@param y number
---@return boolean? ok
---@return string? error
function Creature:order_move(x, y) end

--- How the creature moves now: 1 walk, 2 run (the game's field at +0x1F8; 0: none yet). Pass a value to set it first (0 to 3). The game sets it when an order
--- starts: a creature of the game's own AI runs while it chases (order_attack) and walks after `order_move`, so call `creature:gait(2)` right after
--- `order_move` to make it run (measured on a wolf: 133 units a second walking, 335 running, the movement speed buffs scale both). The hero's own gait
--- follows the Shift key, setting it has no effect there. Single player.
---@param value integer?
---@return integer? gait
---@return string? error
function Creature:gait(value) end

--- Orders a party hero to stop. In single player this also works on any other creature.
---@return boolean? ok
---@return string? error
function Creature:order_stop() end

--- A party hero picks up the world item `item` (an id or a table with `id`) at once, as the game's click and collect behaviour do when
--- they arrive: no walking, the item may be anywhere. Single player.
---@param item integer|GearItem
---@return boolean? ok
---@return string? error
function Creature:pickup(item) end

--- Orders a party hero to attack `target` with its weapon (it walks there first), as a click on an enemy does. In single player
--- (`gear.world.can_change()`) this also works on any other creature, a monster for example: it attacks `target` as a script's
--- order would make it (a creature made by `gear.world.spawn` does not notice heroes on its own, see the spawn-rate mod).
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

--- Adds the art (id from 1000) or spell `ability` to a hero's abilities at `level` (default 1), with the game's own function (a
--- rune read does the same). For heroes of the local player and the party (ids 1 to 16). Returns true, or nil and why
--- (`the hero knows it already`).
---@param ability integer
---@param level? integer
---@return boolean? ok
---@return string? error
function Creature:learn(ability, level) end

--- Raises the level of an art or spell a hero knows by `levels` (default 1) with the game's own function (the one a skill purchase
--- uses). For heroes of the local player and the party (ids 1 to 16). Returns true, or nil and why (`the hero does not know it`,
--- `the level would pass 255`).
---@param ability integer
---@param levels? integer
---@return boolean? ok
---@return string? error
function Creature:raise(ability, levels) end

--- The creature uses an item of its own backpack as a click on it does: a potion is drunk, a rune or scroll read. `item` is an item id
--- or a table with `id`. Returns true, or nil and why.
---@param item integer|table
---@return boolean? ok
---@return string? error
function Creature:use_item(item) end

--- Removes an art or spell from a hero's abilities.
---@param ability integer
---@return boolean? ok
---@return string? error
function Creature:forget(ability) end

--- Sets the name the game shows for a party hero (1 to 30 characters, UTF-8). The game keeps a companion's name only
--- until the next savegame load: a mod sets it again (the player's own hero keeps the name the savegame has).
---@param name string
---@return boolean? ok
---@return string? error
function Creature:set_name(name) end

--- Lets a party hero drink a potion from its own backpack, as the potion keys do for the controlled hero (the game
--- picks the potion). Fails if it carries none. The `item.use` event is not asked for this drink (a mod's Lua is running), the potion has
--- the engine's effect.
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
