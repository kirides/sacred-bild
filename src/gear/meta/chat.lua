---@meta _
-- Sacred Gear: the game's chat and message lines (gear.chat).

-- ## Chat lines (gear.chat)
--: Every line the game adds to its message list at the top of the screen goes to the mods too: chat from other
--: players (to everyone, to the team, private), the game server's messages, the game's own notes (a trade declined,
--: a player joined, ...) and the player's own lines. The `chat.line` event gets each one as it comes; `history` keeps
--: the last 200 of the session, also those that came before the mod loaded. For player chat Gear knows who sent it
--: (from the network message the line was made of). A mod that shows the lines itself (a chat window) can keep the
--: game's own lines off the screen with `replace_overlay`; they come back by themselves when it stops calling it.
--: The lines of a window's own list (the trade window, the private conversation window) are passed on too, unless
--: they are the same line the message list just got.

---@alias GearChatKind
---| 'global' # A player's chat to everyone.
---| 'team' # A player's chat to the team.
---| 'private' # A private message to this player.
---| 'server' # A message of the game server.
---| 'own' # The player's own line, as the game echoes it.
---| 'system' # The game's own lines (and chat whose sender could not be told).
---| 'notice' # The game's separate notices (channel 0x10).

--- A run of a line's text in one colour (the game's `\c` codes switch colours, `\r` goes back).
---@class GearChatRun
---@field text string
---@field color integer ARGB.

--- One line, as `chat.line` and `history` give it. Shared by every mod: read it, do not change it.
---@class GearChatLine
---@field id integer Counts up from 1 in the session.
---@field time number `gear.time()` when it came.
---@field text string As the game got it (UTF-8), with its backslash codes (`\cAARRGGBB` colour, `\r` reset, `\g` image).
---@field plain string The text without the codes.
---@field runs GearChatRun[] The text in runs of one colour.
---@field color integer The line's colour (ARGB): the game's for its channel (white, light blue for private, tomato for the server, green for own lines) unless the line came with one (team chat: yellow).
---@field channel integer The game's channel (1 to 4, 0x10 notices).
---@field kind GearChatKind
---@field main boolean The game's message list (false: a window's own list).
---@field hidden boolean The game did not show it (a mod replaced the overlay).
---@field slot? integer Player chat: the sender's slot (1 to 16, 0 the server).
---@field sender? string Player chat: the sender's name as the game shows it.
---@field message? string Player chat: what was written, without the name the game puts before it.

-- path gear.chat
--- The game's chat and message lines.
---@class GearChat
local GearChat = {}

--- Whether the lines can be read (the game's message list was found in this build).
---@return boolean
function GearChat.available() end

--- The last `n` lines of the session (all that are kept, up to 200, without `n`), oldest first.
---@param n? integer
---@return GearChatLine[]
function GearChat.history(n) end

--- Call it every tick the mod shows the lines itself: the game then adds no line to its own message list (its chat
--- and message lines at the top of the screen stay empty; the lines still reach `chat.line`). The game's lines come
--- back half a second after the last call, so a mod that is switched off, stopped by an error or not drawn gives them
--- back by itself. Several mods may call it. Returns `available()`.
---@return boolean available
function GearChat.replace_overlay() end

--- A text with the game's backslash codes taken apart: its runs of one colour (`color` where no code says
--- otherwise, default white) and the plain text. Also for item names from `gear.ui.item_tooltip_text`.
---@param text string
---@param color? integer
---@return GearChatRun[] runs
---@return string plain
function GearChat.segments(text, color) end
