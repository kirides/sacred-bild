---@meta _
-- Sacred Gear: item drops.

-- ## Drops (gear.drops)

--- The event table of `drops.roll`: a creature's loot function is about to run. Fields you set are read back; the others are for information.
---@class GearDropRoll
---@field creature integer The dying creature's type (an index into `gear.data.object_types`).
---@field level integer Its level.
---@field forced integer The item type of an authored drop (the creature's drop list, a quest item ...), 0 for a random drop.
---@field bonus integer The rarity bonus the caller passes (it widens the weights of the rarer qualities; the loot code caps it at 700). Set it to change it.
---@field killer integer The killer's object id (a hero: 1 .. 16), 0 when unknown.
---@field survival number The caller's survival value (0 for most callers).
---@field network boolean True in a network game.
---@field cancel? boolean Set by the kernel when a handler returned `false`: the roll does not run.

--- The event table of `drops.select`: the loot function asks the equipment type picker for an item. Change `category`, `level` or `quality`
--- to change what the engine picks, or set `type` to use an item type of your own instead of its pick.
---@class GearDropSelect
---@field category integer The equipment category the loot code chose: 5 weapon, 6 chest armour, 8 ring, 13 shield, 17 helmet, 18 boots, 19 belt, 20 amulet, 21 shoulders, 22 arms, 23 legs, 24 gloves, 25 wings, 29 horse equipment.
---@field level integer The level the item is picked for (the creature's level).
---@field quality integer The quality (0 plain, 1 .. 14 the rarer tiers; the rarity roll's result as `16 - id`).
---@field hero_pref integer The hero type the picker prefers (0 none), for information.
---@field flags integer The picker's flags (0, or 2 when the loot code prefers a class), for information.
---@field creature integer The dying creature's type.
---@field type? integer Set it to an item type id (1 .. 0x1FFF) to use that item instead of the engine's pick.

--- A row of the game's rarity table.
---@class GearRarityRow
---@field id integer The table id (2 .. 15 are the qualities 14 .. 1, 16 is plain).
---@field quality integer The quality the id gives (`16 - id`, 0 for 16).
---@field weight integer The weight now (after the mods' multipliers).
---@field vanilla integer The weight as the game loaded it.

--- Counters of the drop hooks since the start (or `reset_stats`).
---@class GearDropStats
---@field rolls integer Loot function runs (every creature death with loot, forced or random).
---@field forced integer Of them with an authored item type.
---@field vetoed integer Rolls a `drops.roll` handler cancelled.
---@field picks integer Equipment type picks inside loot rolls.
---@field overridden integer Picks a `drops.select` handler replaced with its own type.
---@field rarity_draws integer Draws of the rarity roll (1 .. 10000) of all five rarity paths.
---@field rand_calls integer Calls of the game's `rand` inside the loot function that the chosen generator answered.
---@field lua_draws integer Of them answered by a Lua generator.
---@field quality table<integer, integer> Picks by quality 0 .. 15, after the handlers.
---@field category table<integer, integer> Picks by category 0 .. 31.

-- path gear.drops
--- Tuning what creatures drop. Nothing here changes the game until a mod calls it: with no mod the engine's own code runs. The settings of
--- every mod are merged (multipliers multiply; the first mod in load order that chose a generator wins) and applied after each tick, so a
--- setting reaches the next drop. The loot code runs in the host game (a single player game, or the player who owns the creature in a
--- network game); GameServer.exe contains none of it. See `docs/GearDrops.md` for what the engine does and what each setting touches.
---@class GearDrops
local GearDrops = {}

--- Multiplies the chance of every drop attempt of creatures (the loot code's `budget / 1000` per attempt; above 1000 it runs more passes). `nil`
--- or 1: as the game has it. Authored drops (a creature's drop list) are not affected.
---@param multiplier number? 0 .. 1000
function GearDrops.chance_scale(multiplier) end

--- Multiplies how strongly the rarity bonus ("magic find") widens the weights of the rarer qualities. `nil` or 1: as the game has it.
---@param multiplier number? 0 .. 1000
function GearDrops.find_scale(multiplier) end

--- Multiplies the weights of the rarity table by quality (replaces this mod's previous multipliers; `nil` clears them). Keys are the qualities
--- 0 (plain, also `'normal'`) and 1 .. 14; a quality you leave out keeps its weight. The table feeds every path that rolls a rarity (creature loot,
--- containers, the other equipment generators), and the plain weight does not grow with the rarity bonus while the others do. If the weights add up to less than
--- 10000 the remainder is plain items; more than 10000 pushes the rarest qualities out of reach.
---@param weights table<integer|string, number>?
function GearDrops.rarity_weights(weights) end

--- Chooses the generator of the loot rolls (the chances, the kind of item, the rarity roll): `'fair'` is PCG32 without modulo bias (the game's
--- `rand() % 10000` for the rarity roll favours the first 27.7% of the range by a third), `{seed = n}` the same with a seed (the same rolls every
--- run), a function returns a number in [0, 1) per draw (called a few dozen times per loot roll, keep it cheap), `nil` or `'game'` the game's own.
---@param spec 'fair'|'game'|{seed: number}|(fun(): number)|nil
function GearDrops.rng(spec) end

--- A number 0 .. n - 1 from the fair generator, for your own rolls.
---@param n number
---@return integer
function GearDrops.random(n) end

--- The game's rarity table now (empty before the first game ran).
---@return GearRarityRow[]
function GearDrops.rarity_table() end

--- Counters of the drop hooks.
---@return GearDropStats
function GearDrops.stats() end

--- Sets the counters to zero.
function GearDrops.reset_stats() end

--- Makes the engine run the loot function `count` times for a creature (an object id), a few dozen per frame, through all hooks; the items lie
--- in the world afterwards. A tuning and test aid: a single player game only.
---@param creature integer
---@param count integer
---@param bonus? integer The rarity bonus to roll with (default 0).
---@return boolean? ok
---@return string? message
function GearDrops.simulate(creature, count, bonus) end

--- Rolls done and queued by `simulate`.
---@return integer done
---@return integer queued
function GearDrops.simulation_progress() end
