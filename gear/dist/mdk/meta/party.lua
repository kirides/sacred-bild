---@meta _
-- Sacred Gear: party companions in single player.

-- ## Party (gear.party)
--: Companions are more heroes of the player, at the hero ids 3 to 16, the way a network game holds the players'
--: heroes: each has its own backpack, equipment, attributes, skills, combat arts, spells and experience. The player
--: controls one of them at a time (`control`); the others stand where they are until a mod gives them orders
--: (`creature:order_move`, `order_attack`, `order_ability`). Companions are saved with the savegame like everything
--: in the world. Single player only: in a network game the functions refuse. Loading a savegame gives control back
--: to the hero with id 1.

-- path gear.party
---@class GearParty
---@field FIRST_ID integer The first companion id (3; id 2 is never used: the engine moves object 2 with the arrow keys in single player).
---@field LAST_ID integer The last hero id (16).
local GearParty = {}

--- Whether the party works now: a single player game with a hero.
---@return boolean ok
---@return string? reason Why not.
function GearParty.available() end

--- The player's heroes in the world, the controlled one first (never another player's hero in a network game).
---@return Creature[]
function GearParty.heroes() end

--- The hero the player controls (the same as `gear.world.hero()`).
---@return Creature?
function GearParty.leader() end

--- The heroes the player does not control right now.
---@return Creature[]
function GearParty.companions() end

--- Creates a companion of `class` where the leader stands (or at `opts.x`, `opts.y`), as the engine creates the
--- player's own hero, with the lowest free id from 3 on. It starts at level 1 without items; `opts.experience` is
--- given through the game's own experience gain, so it levels up with skill points to spend.
---@param class GearClassName|integer
---@param opts? GearRecruitOpts
---@return Creature? companion
---@return string? error `'the party is full'` (ids 3 to 16 taken), or why not.
function GearParty.recruit(class, opts) end

--- Removes a companion from the world for good, with what it wears. Refused for the controlled hero and while its
--- backpack holds items (take them first).
---@param companion Creature|integer
---@return boolean? ok
---@return string? error
function GearParty.dismiss(companion) end

--- Gives the player control of a party hero: the camera, the HUD, the windows (inventory, skills, combat arts), the
--- mouse, the keys and the controller then serve it, and the hero ids of the game follow (`gear.world.hero()`). The
--- gold goes with control (the party's purse), and the mercenaries follow the controlled hero. Every mod gets
--- `party.control` `(from, to)` afterwards.
---@param hero Creature|integer
---@return boolean? ok
---@return string? error
function GearParty.control(hero) end

--- Experience a party hero gains from the game (kills, quests) also goes to the other living party heroes within
--- `radius` world units (0 or nil: anywhere), times `share` (1: the same amount, 0: off, the default).
---@param share number
---@param radius? number
function GearParty.share_experience(share, radius) end

---@class GearRecruitOpts
---@field x? number Where to create it (default: where the leader stands).
---@field y? number
---@field layer? integer
---@field experience? number Experience to start with, given through the game's own gain (levels up with skill points).

--- What `creature:abilities()` lists.
---@class GearHeroAbility
---@field id integer The ability: combat arts from 1000, spells below (`gear.data.abilities[id]`).
---@field level integer The level from read runes.
---@field bonus integer Levels from items.
---@field regen number Seconds until it can be used again (0: ready).
---@field regen_max number Its regeneration time in seconds.
---@field art boolean A combat art (id from 1000).
---@field ready boolean `regen` is 0.

--- What `creature:combos()` lists.
---@class GearHeroCombo
---@field id integer What `order_ability` takes (0x200 + slot).
---@field slot integer 0 to 3.
---@field arts integer[] The combat arts, in order.
---@field levels integer[] Their levels.
---@field total number The regeneration time of the combo (the arts' together), seconds.
---@field charge number How far it has regenerated.
---@field ready boolean `charge >= total`.

---@alias GearHeroAction
---| 'idle'
---| 'walking'
---| 'attacking'
---| 'ability' # Using a combat art or spell.
---| 'busy' # Something else (a state the API does not name).
