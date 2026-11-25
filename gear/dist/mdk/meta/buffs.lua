---@meta _
-- Sacred Gear: buffs, debuffs and auras on any creature.

-- ## Buffs (gear.buffs)
--: A buff changes a creature's combat values for a while: its resistances, damage bonuses and factors (armour, attack,
--: speed, regeneration). The game works these values out anew whenever something changes (level, items, its own
--: buffs); Gear adds what the creature's Gear buffs give each time, so they hold through equipment changes and go
--: back when the buff ends or is removed. A buff can also show one of the game's spell overlays on the creature (the
--: stone of Steinhaut, the flames of Flammenhaut, the light of Lichtschild) while it runs. A debuff is a buff with
--: negative values. Definitions are plain data shared by every mod (`definitions` lists them, for editors); a mod
--: applies one to any creature at a strength (0.5: half the values, factors halfway to 1). Buffs are not saved:
--: loading a savegame or starting a new game ends them all (`buff.ended`).
--:
--: To pass on a buff the game gave (a companion's Steinhaut onto the hero you play): take `fields` of the caster
--: before and after its cast, `change` turns the difference into stats and factors, `apply` them elsewhere.

-- path gear.buffs
---@class GearBuffs
---@field STATS table<string, integer> The values a buff adds to, by name: `resist_physical`, `resist_fire`, `resist_magic`, `resist_poison`, `damage_min_physical` / `_fire` / `_magic` / `_poison`, `damage_max_physical` / `_fire` / `_magic` / `_poison` (their stats block offsets).
---@field FACTORS table<string, integer> The values a buff multiplies, by name: `attack_factor`, `speed_factor`, `armor_factor`, `regen_factor`, `art_regen_factor`.
---@field SPEEDS table<string, integer> The speed values (attack, cast, movement), by name: `attack_speed`, `cast_speed`, `move_speed` (their stats block offsets). 100 is normal speed; the animation runs value / 100 as fast. The engine caps each at 220 (260 for creatures with flag 0x10000, movement 380 for some); a buff's `speeds` are added after the cap, so they go beyond it. Values are clamped to 0..1000.
---@field OVERLAYS table<string, integer> The game's spell overlays: `light` (6, Lichtschild), `stone` (7, Steinhaut), `flame` (10, Flammenhaut).
local GearBuffs = {}

--- A buff's definition.
---@class GearBuffDef
---@field id string `'name'` (this mod's: becomes `'mod_id:name'`) or `'other_mod:name'`.
---@field name? string Shown to the player.
---@field description? string
---@field icon? string The game's texture name of its icon (for lists and editors).
---@field stats? table<string|integer, number> What it adds: `{resist_fire = 50}` (names of `STATS`, or stats block offsets).
---@field factors? table<string|integer, number> What it multiplies: `{armor_factor = 1.5}` (names of `FACTORS`, or offsets).
---@field speeds? table<string|integer, number> What it adds to the speed values, past the engine's cap of 220: `{move_speed = 80}` (names of `SPEEDS`, or offsets).
---@field speed_factors? table<string|integer, number> What it multiplies them by, past the cap: `{attack_speed = 2}` (names of `SPEEDS`, or offsets).
---@field overlay? string|integer The spell overlay shown while it runs: a name of `OVERLAYS` or its number.
---@field seconds? number How long it lasts (0 or nil: until removed).
---@field debuff? boolean Marks a harmful one (for lists; the values decide what it does).

--- What `apply` can be told.
---@class GearBuffOpts
---@field strength? number 1 (default): as defined; 0.5: half the values and factors halfway to 1.
---@field seconds? number Instead of the definition's.
---@field key? string One buff per key and creature (default: the definition's id): the same key again replaces it.
---@field source? Creature|integer Who gave it.
---@field ability? integer With which combat art or spell.
---@field overlay? integer Instead of the definition's.

--- A running buff (`list`).
---@class GearBuff
---@field key string
---@field id string Its definition's id.
---@field strength number
---@field seconds number How long it was given for (0: until removed).
---@field left number Seconds left (0 when it has no end).
---@field overlay integer
---@field source? Creature
---@field ability? integer
---@field mods {[1]: integer, [2]: boolean, [3]: number}[] What it does: stats block offset, factor (else added), value.

--- Defines (or replaces) a buff. Returns the definition with its full id.
---@param def GearBuffDef
---@return GearBuffDef
function GearBuffs.define(def) end

--- Every defined buff, in the order they were defined.
---@return GearBuffDef[]
function GearBuffs.definitions() end

--- Gives `target` a buff: a definition's id (`'stone'`, `'other_mod:stone'`) or a definition table (defined on the
--- spot).
---@param target Creature|integer
---@param buff string|GearBuffDef
---@param opts? GearBuffOpts
---@return boolean? ok
---@return string? error
function GearBuffs.apply(target, buff, opts) end

--- Ends a buff before its time.
---@param target Creature|integer
---@param key string
---@return boolean found
function GearBuffs.remove(target, key) end

--- The creature's running Gear buffs.
---@param target Creature|integer
---@return GearBuff[]
function GearBuffs.list(target) end

--- The creature's combat values now, by stats block offset (`{[0x66] = 12.5, ...}`; the offsets of `STATS` and `FACTORS`
--- and more). nil when it is no creature.
---@param target Creature|integer
---@return table<integer, number>?
function GearBuffs.fields(target) end

--- The creature's speed values now, by name (`{attack_speed = 140, cast_speed = 120, move_speed = 130}`), beyond the
--- engine's cap when a buff lifted it. nil when it is no creature.
---@param target Creature|integer
---@return table<string, integer>?
function GearBuffs.speeds(target) end

--- What changed between two `fields` snapshots, as `stats` and `factors` of a definition (by offset).
---@param before table<integer, number>
---@param after table<integer, number>
---@return table<integer, number> stats
---@return table<integer, number> factors
function GearBuffs.change(before, after) end

--- The spell overlay the creature shows and its seconds left (the game's own buffs set it too).
---@param target Creature|integer
---@return integer? overlay
---@return number? seconds
function GearBuffs.overlay(target) end

--- Shows a spell overlay on the creature for `seconds` (default 30; 0 or `which` 0: none). The game counts it down.
---@param target Creature|integer
---@param which string|integer
---@param seconds? number
---@return boolean ok
function GearBuffs.set_overlay(target, which, seconds) end

---@class Creature
local Creature = {}

--- The creature's running Gear buffs (`gear.buffs.list`).
---@return GearBuff[]
function Creature:buffs() end

--- Ends one of its Gear buffs (`gear.buffs.remove`).
---@param key string
---@return boolean found
function Creature:remove_buff(key) end
