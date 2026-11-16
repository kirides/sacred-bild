---@meta _
-- Sacred Gear: multiplayer.

-- ## Multiplayer (gear.mp)
--: In a network game every player is a hero object (slots 1 to 16 are the object ids of the heroes). A mod runs on
--: every player's computer; the `kill` event runs on the computer of the player who made the kill. Design and what is
--: still to come (chat, a channel between the mods, per-player storage, announcements): `docs/GearServerMods.md`.

--- A player of the game.
---@class GearPlayer
---@field slot integer The player's slot, 1 to 16; the same number as the hero's object id.
---@field class GearClassName? The hero's class.
---@field is_local boolean This is the player at this computer.
local GearPlayer = {}

--- The player's hero.
---@return Creature
function GearPlayer:hero() end

-- path gear.mp
--- Players of a network game.
---@class GearMp
local GearMp = {}

--- The players whose heroes are in the game, by slot. Empty before a game runs.
---@return GearPlayer[]
function GearMp.players() end

--- The player in `slot` (1 to 16), or nil.
---@param slot integer
---@return GearPlayer?
function GearMp.player(slot) end

--- The player at this computer, or nil.
---@return GearPlayer?
function GearMp.local_player() end

--- Whether the heroes of other players are in the game (a network game with more than the local player).
---@return boolean
function GearMp.active() end

--- What the `kill` event gets.
---@class GearKill
---@field victim Creature The creature that died (it is retired a moment later; read what you need at once).
---@field type integer? Its creature type (nil if the object is already gone).
---@field type_name string? The name of that type.
---@field killer GearPlayer? The player who killed it; nil if a creature or something else did.
---@field award integer Experience the killer got.
---@field by_local boolean The local hero killed it.
