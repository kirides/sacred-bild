---@meta _
-- Sacred Gear: custom net messages.

-- ## Custom net messages (gear.net)
--: Named channels between the mods of different computers (and the game server's scripts) over two message types of
--: their own, above the game's range. They carry messages up to 256 KB, as bytes or tables (JSON), cut into pieces and
--: paced by the library. A computer says HELLO when it joins; a server that supports them (the Sacred Gear game
--: server, `Sacred.GameServer` of the SacredEngineRemake) answers with its limits and the list of players who can use
--: them. The original `GameServer.exe` and hosts without it do not answer: `gear.net.supported()` stays false and
--: `gear.mp.send` keeps using the chat carrier, which every server relays. `gear.mp.send` uses the custom messages by
--: itself for players that can; mods of the auction house kind need no change.
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

--- Sends `payload` (a string of bytes, or a table sent as JSON, at most `gear.net.limits().max_message` bytes, 256 KB by
--- default) to `to`: `'server'` (the game server's scripts), `'host'` (the default), `'all'` (everybody else) or a
--- slot or player. The pieces leave over the next ticks, paced to the server's rate (64 KB/s by default); messages
--- of one sender arrive in order. Only players that can use the custom messages get them (`gear.net.peer_supported`);
--- the others are skipped silently. Not available (nil and the reason) when the server does not support the messages.
---@param payload table|string
---@param to? integer|GearPlayer|'server'|'host'|'all'
---@return boolean? ok
---@return string? error `'custom net messages are not available here ...'`, a bad target, `'too many messages are waiting to be sent'`, a message that is too large ...
function GearNetChannel:send(payload, to) end

--- What the `net_caps` event gets.
---@class GearNetCaps
---@field supported boolean The server supports the custom messages (the handshake is done).
---@field peers integer[] The slots of the players that can use them (this computer's slot included).

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

--- Whether the game server supports the custom messages (the handshake after joining is done). False in single player,
--- before the answer and on servers without them.
---@return boolean
function GearNet.supported() end

--- Whether the player in `slot` (1 to 16) can use them (told by the server; this computer's slot included).
---@param slot integer
---@return boolean
function GearNet.peer_supported(slot) end

--- The server's limits, nil while `supported()` is false.
---@return GearNetLimits?
function GearNet.limits() end

--- Sets the two message types (`control`, `data`: above 0x1D5 and below 0x400, different) for servers that use
--- other numbers than 0x1E0 / 0x1E1. Only before the game connects (nil and a message once the handshake started).
---@param opts GearNetConfigureOpts
---@return boolean? ok
---@return string? error
function GearNet.configure(opts) end
