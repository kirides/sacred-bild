---@meta _
-- Sacred Gear: custom net messages.

-- ## Custom net messages (gear.net)
--: Named channels between the mods of different computers (and the game server's scripts). A channel works everywhere: its
--: messages ride on the chat carrier (private chat frames every server relays, up to 64 KB, about 8 KB/s) and, where the
--: server agrees, on two message types of their own above the game's range (up to 256 KB, faster). The agreement is made
--: on the chat carrier first: a server that supports channels (the Sacred Gear game server, `Sacred.GameServer` of the
--: SacredEngineRemake) sends a query with the two numbers it offers when the player enters the game, the client answers
--: over chat, and one HELLO / HELLO_ACK on the agreed types checks the versions and limits. Nothing custom is ever sent to a
--: server that did not ask, so the original `GameServer.exe` and servers with opcodes of their own see only chat. Without
--: the agreement (or when it fails) messages to players still go over chat; messages to `'server'` need a server that
--: supports channels (`gear.net.transport('server')` is nil otherwise). `gear.mp.send` uses the custom messages by itself
--: for players that can. `gear.net.transport` says which way a message would travel.
--:
--: Every message names a channel (`'auction:bid'`); both sides ask for the same name. A channel's id is the FNV-1a hash
--: of its name; two names with the same id are refused when the second is created. Messages for channels this computer
--: has not created are ignored. The server puts the real sender into the message: the `from` a handler gets cannot be
--: forged by the sender. Nothing is encrypted and a modified client can send anything: check what you receive.
--:
--: The two message types default to 0x1E0 (control) and 0x1E1 (data). A server with custom types of its own announces
--: the data type itself; the control type is set with `gear.net.configure` before the game connects, with the
--: environment variable `GEAR_NETOPS="0x1E0,0x1E1"`, or in the server's `NETOPS_CONTROL` / `NETOPS_DATA`.

--- A named channel (`gear.net.channel`).
---@class GearNetChannel
---@field name string The channel's name.
---@field id integer Its id on the wire (FNV-1a of the name).
local GearNetChannel = {}

--- Calls `fn(from, payload)` for every message that arrives on the channel. `from` is the sending player (slot 0 is the
--- server); `payload` is a table if the sender sent one (JSON) and a string of bytes otherwise. An error in `fn` is
--- logged and does not reach the game. Returns `fn`, for `GearNetChannel:off`.
---@param fn fun(from: GearPlayer, payload: table|string)
---@return fun(from: GearPlayer, payload: table|string) fn
function GearNetChannel:on(fn) end

--- Removes a handler added with `GearNetChannel:on`. Returns whether it was there.
---@param fn fun(from: GearPlayer, payload: table|string)
---@return boolean removed
function GearNetChannel:off(fn) end

--- Sends `payload` (a string of bytes, or a table sent as JSON; at most 64 KB over the chat carrier, `gear.net.limits().max_message`,
--- 256 KB by default, over the custom messages) to `to`: `'server'` (the game server's scripts), `'host'` (the default),
--- `'all'` (everybody else) or a slot or player. Each recipient gets it the best way: over the custom messages when this
--- computer and the player can use them (`gear.net.peer_supported`), else over the chat carrier. The pieces leave over the next
--- ticks, paced (64 KB/s custom, about 8 KB/s chat); messages of one sender arrive in order per transport.
---@param payload table|string
---@param to? integer|GearPlayer|'server'|'host'|'all'
---@return boolean? ok
---@return string? error `'the server does not support net channels'`, a bad target, `'too many messages are waiting to be sent'`, a message that is too large ...
function GearNetChannel:send(payload, to) end

--- Which way a message to `to` would travel now (same `to` as `GearNetChannel:send`): `'custom'`, `'chat'`, `'mixed'` (some
--- players one way, some the other) or nil when it cannot be sent.
---@param to? integer|GearPlayer|'server'|'host'|'all'
---@return 'custom'|'chat'|'mixed'|nil transport
function GearNetChannel:transport(to) end

--- What the `net_caps` event gets.
---@class GearNetCaps
---@field supported boolean The server supports the custom messages (the handshake is done).
---@field peers integer[] The slots of the players that can use them (this computer's slot included).
---@field chat boolean The server supports net channels (it queried this client), at least over the chat carrier.

--- What `gear.net.limits` returns: the server's announced limits.
---@class GearNetLimits
---@field max_fragment integer Bytes of one wire message (the library cuts messages to this).
---@field max_message integer Largest message.
---@field bytes_per_second integer The rate the server accepts from this computer.
---@field in_flight integer Messages of one sender the server follows at once.

--- Options of gear.net.configure.
---@class GearNetConfigureOpts
---@field control? integer The control message type (default 0x1E0).
---@field data? integer The data message type (default 0x1E1).
local GearNetConfigureOpts = {}

-- path gear.net
--- Custom net messages: named channels between mods on different computers and the game server's scripts.
---@class GearNet
local GearNet = {}

--- The channel `name` (1 to 64 printable ASCII characters; use your mod's id as a prefix, `'auction:bid'`). The same
--- name returns the same channel. nil and a message if the name is bad or has the same id as another channel's name.
--- `opts` is reserved.
---@param name string
---@param opts? table
---@return GearNetChannel? channel
---@return string? error
function GearNet.channel(name, opts) end

--- Whether the custom message types are in use with the game server (the chat query, the answer and HELLO / HELLO_ACK are
--- done). False in single player, before the agreement, and on servers without them; channels still work over chat then
--- (see `gear.net.transport`).
---@return boolean
function GearNet.supported() end

--- Whether the player in `slot` (1 to 16) can use them (told by the server; this computer's slot included).
---@param slot integer
---@return boolean
function GearNet.peer_supported(slot) end

--- Which way a message to `to` would travel now: `'custom'`, `'chat'`, `'mixed'` or nil when it cannot be sent (no such
--- player, `'server'` while the server does not support net channels).
---@param to? integer|GearPlayer|'server'|'host'|'all'
---@return 'custom'|'chat'|'mixed'|nil transport
function GearNet.transport(to) end

--- The server's limits, nil while `supported()` is false.
---@return GearNetLimits?
function GearNet.limits() end

--- Restricts the two message types (`control`, `data`: above 0x1D5 and below 0x400, different) a server may offer: the client
--- accepts only these numbers and declines others (channels then stay on chat). Without it the offered numbers are accepted.
--- Only before the game connects (nil and a message once the handshake started).
---@param opts GearNetConfigureOpts
---@return boolean? ok
---@return string? error
function GearNet.configure(opts) end
