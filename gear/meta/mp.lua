---@meta _
-- Sacred Gear: multiplayer.

-- ## Multiplayer (gear.mp)
--: In a network game every player is a hero object (slots 1 to 16 are the object ids of the heroes). A mod runs on
--: every player's computer; the `kill` event runs on the computer of the player who made the kill. Mods on different
--: computers talk through the mod channel (`gear.mp.send` and the `mp_message` event), which rides the game's own
--: connection. How it works and the messages it builds on: `docs/GearMultiplayer.md`.
--:
--: The host is the player whose game started the server. In single player the local player is the host and slot 1.

--- A player of the game.
---@class GearPlayer
---@field slot integer The player's slot, 1 to 16; the same number as the hero's object id. 0 for the server (`mp_message` only).
---@field name string The hero's name as the game shows it (`'hero <slot>'` if the game has none for it).
---@field key string An identity for storage: the name in lower case, `:` and the class (`'aria:seraphim'`); opaque, stable between sessions, not unique across computers.
---@field class GearClassName? The hero's class.
---@field is_local boolean This is the player at this computer.
---@field is_host boolean This player is the host (started the game). Always true for the local player in single player.
local GearPlayer = {}

--- The player's hero.
---@return Creature
function GearPlayer:hero() end

-- path gear.mp
--- Players of a network game and the mod channel.
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

--- The local player's slot (the local hero's object id); 1 in single player, nil in a network game before the hero is there.
---@return integer?
function GearMp.local_slot() end

--- The host's slot; the local slot in single player, nil in a network game while the host is not known yet.
---@return integer?
function GearMp.host_slot() end

--- The kind of game: `'single'` (no network game), `'lan'` (the multiplayer menu's LAN), `'open'` (Open Internet, characters on
--- the players' computers) or `'closed'` (Closed Internet: the heroes belong to lobby accounts and the server keeps them).
--- Default-deny: a game entered through the Internet menu is `'closed'` unless Open Net is proven. Read from the engine, not
--- from anything a mod or the player can set: the net object's bytes ("the Internet menu was used", "lobby identity checked")
--- and the hero's source: in Closed Net the server delivers the hero (message 0x132); in Open Net none comes, so the game
--- reads `'open'` once the hero has been in the game for 10 s with the receive hook in and none arrived. `'lan'` only when
--- entered through the LAN menu. Anything that cannot be read counts as `'closed'`.
---@return 'single'|'lan'|'open'|'closed' mode
function GearMp.mode() end

--- Whether this computer is the host (always true in single player).
---@return boolean
function GearMp.is_host() end

--- Sends `payload` to the mods on another computer (or this one), which get it as the `mp_message` event
--- `(from, channel, payload)`. `channel` names what the message is for (1 to 32 letters, digits, `_`, `.`, `-`; use
--- your mod's id). `payload` is a table (sent as JSON, `gear.json` rules) or a string, at most 64 KB. `to` is a slot,
--- a player, `'host'` (the default) or `'all'` (every player, this one included).
---
--- Messages to this computer (single player, or the host sending to itself) arrive on the next tick without the
--- network. Others travel as frames of the game's private chat, which every game server relays; players never see
--- them. Sending is paced (about 8 KB/s), so a 64 KB message takes about 8 s; messages to one player arrive in the
--- order they were sent. Incomplete messages (a player left mid-message) are dropped after 30 s. Nothing is
--- encrypted, and a modified client can send anything: check what you receive.
---@param channel string
---@param payload table|string
---@param to? integer|GearPlayer|'host'|'all'
---@return boolean? ok
---@return string? error `'the host is not known yet'`, `'no such player'`, `'too many messages are waiting to be sent'`, a bad channel or payload ...
function GearMp.send(channel, payload, to) end

--- Debug: `true` sends messages to the local slot through the real chat carrier (out to the server and back) instead
--- of the loopback, in a network game. For self tests of the channel; off by default.
---@param on boolean
function GearMp.force_network(on) end

--- What the `kill` event gets.
---@class GearKill
---@field victim Creature The creature that died (it is retired a moment later; read what you need at once).
---@field type integer? Its creature type (nil if the object is already gone).
---@field type_name string? The name of that type.
---@field killer GearPlayer? The player who killed it; nil if a creature or something else did.
---@field award integer Experience the killer got.
---@field by_local boolean The local hero killed it.
