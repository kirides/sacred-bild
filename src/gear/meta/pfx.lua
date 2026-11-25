---@meta _
-- Sacred Gear: skill particle effects.

-- ## Skill effects (gear.pfx)
--: The game has no effect table a mod can add to. Every skill effect is an object type (0x300 to 0x49D) that the game's effect setup
--: turns into one of about 180 particle system classes, each with its colours, textures and timings in its own code. Sacred Gear
--: leaves those classes alone and gives a mod two things on top: a **variant**, which is an engine effect that looks and runs
--: differently, and **rules** that pick a variant while an art or spell makes its effect.
--:
--: A variant is a set of changes to an engine effect: other textures (names from the game's own texture list, e.g.
--: `PARTICLE_GLOW01.TGA`), a colour change of every sprite (`hue` turns the hue, `colorize` makes it that colour with its own brightness,
--: `multiply` multiplies, `alpha` fades) and a `speed` (0.5: the effect lasts twice as long). The id is the mod's own number (1 and up,
--: shared by every mod like ability aliases: pick a range that is yours) and is what rules and the `pfx.select` handlers name.
--:
--: A rule says when: `fx` (an effect type or list; without it every effect), `ability` (the art or spell in flight, an alias too), and
--: the cast level (`min_level`, `max_level`), then `use` a variant, or
--: `select` a function that gets the context and returns one. A handler of `gear.on('pfx.select', function(ctx) ... end)` does the same
--: in code. The context: `fx`, `creature` (the creature whose art or spell state machine makes the effect, nil for an effect the engine
--: made on its own), `ability`, `level` (the cast level), `parent` (the variant of the effect that made this one).
--:
--: Limits: a variant is chosen when the effect's first frame runs, so a rule decides per effect. An effect made by a variant's effect takes the parent's variant.
--: Effects the engine made on its own have no `creature`. The look is local: in a network game other players
--: see the engine's own effect. The textures are the ones already in the game's texture list (`docs/GearPfx.md` has the names).

-- path gear.pfx
---@class GearPfx
local GearPfx = {}

--- A variant.
---@class GearPfxDef
---@field id integer The mod's number for it (1 and up, whole).
---@field name? string Shown in lists.
---@field textures? table<string, string>|string[][] Replacement texture names for the sprites of its effect: `{['PARTICLE_GLOW01.TGA'] = 'PARTICLE_SMOKE01.TGA'}` or `{{from, to}, ...}`. Names of the game's own texture list (`docs/GearPfx.md`); a name the effect does not use changes nothing (the log says so).
---@field hue? number Degrees added to the hue of every sprite's colour (-120: green turns red).
---@field colorize? integer 0xRRGGBB: every sprite takes this colour at its own brightness (white stays white).
---@field multiply? integer 0xAARRGGBB multiplied into every sprite colour.
---@field alpha? number Multiplies the alpha (0 to 4).
---@field speed? number The effect runs this much faster (0.01 to 100; 0.5: twice as long).

--- A rule: when an effect gets a variant.
---@class GearPfxRule
---@field fx? integer|integer[] The engine effect types it applies to (default: all).
---@field ability? integer|integer[] The art or spell (or alias) in flight, for casts.
---@field min_level? integer The cast level (the effect object's level word), at least.
---@field max_level? integer At most.
---@field use? integer The variant to take.
---@field select? fun(ctx: GearPfxContext): integer? Instead of `use`: the variant to take, or nil to go on with the next rule.
---@field priority? integer Higher first (default 0).

--- What `pfx.select` handlers and `select` functions are given.
---@class GearPfxContext
---@field fx integer The engine effect type being set up.
---@field creature? Creature The creature whose art or spell makes it (nil: the engine's own, e.g. a projectile's hit).
---@field ability? integer The art or spell it has in flight.
---@field level? integer The level the effect was cast at (the effect object's level word, or the creature's level of the ability for the first effects of a cast, which come before it is set); nil if unknown.
---@field parent? integer The variant of the effect that made this one (it is taken when no rule picks another).

--- Defines (or replaces) a variant. Returns its id. Raises an error for a bad definition.
---@param def GearPfxDef
---@return integer id
function GearPfx.define(def) end

--- Removes a variant (systems that carry it go back to the engine's look).
---@param id integer
---@return boolean found
function GearPfx.undefine(id) end

--- Every defined variant, by id.
---@return GearPfxDef[]
function GearPfx.definitions() end

--- Adds a rule; the handle is for `remove_rule`.
---@param rule GearPfxRule
---@return table handle
function GearPfx.rule(rule) end

--- Removes a rule.
---@param rule table|integer
function GearPfx.remove_rule(rule) end

--- For tests: how many particle systems carry a variant now and how often a variant was applied.
---@return {live: integer, applied: integer}
function GearPfx.stats() end
