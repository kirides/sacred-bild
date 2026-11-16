---@meta _
-- Sacred Gear: the game itself, saving.

-- ## Saving (gear.game)

-- path gear.game
--- Saving the game through the engine's own save request, the one the savegame window sends: the file is
--- `Save\GAME<slot>.PAK` (slot 0 is the quicksave; the game lists the savegames sorted by file name). Single player
--- only. A save is a request: the engine writes the file a moment later on its render thread, and the result comes as
--- the events `game.saved` and `game.save_failed`. Slots 1 to 99 show as `GAME01` to `GAME99`; a mod that saves on
--- its own should keep to slots the player does not use (the game's savegame window picks the first free number
--- for manual saves).
---@class GearGame
local GearGame = {}

--- The difficulty level of the running game: 0 (bronze) to 4 (niobium); nil if the game is not running.
---@return integer?
function GearGame.difficulty() end

--- Sends the game's UI a click at the current cursor position, as ReBorn's quick cast does after a skill slot was chosen: the chosen
--- skill is used where the cursor is. False if the UI is not there.
---@param down? boolean true (default): a mouse button down event, the class ReBorn's cave sends; false: a button up event.
---@return boolean sent
function GearGame.cursor_click(down) end

--- Whether a save may start now: a living hero in a single player game, the game itself shown (no cinematic, no
--- window drawn alone, no escape menu or message box), no save or load pending.
---@return boolean ok
---@return string? reason Why not.
function GearGame.can_save() end

--- What `save` can be told.
---@class GearSaveOpts
---@field thumbnail? boolean Let the engine capture the savegame's picture first (default false: the savegame keeps no new picture).

--- Asks the engine to save into slot `slot` (0 to 509). The engine refuses while a trading, chest or similar window
--- is open. Returns true when the request is accepted; the outcome is the event `game.saved` or `game.save_failed`.
---@param slot integer
---@param name? string The savegame's display name, at most 60 ASCII characters.
---@param opts? GearSaveOpts
---@return boolean? ok
---@return string? error
function GearGame.save(slot, name, opts) end

--- Whether a save (or a load) is queued or being written.
---@return boolean
function GearGame.save_pending() end

--- The file of a slot.
---@class GearSaveInfo
---@field exists boolean
---@field size number Bytes.
---@field mtime number Last write, unix seconds.

--- The savegame file of `slot`: nil for a slot outside 0..509, else a table whose `exists` says if the file is there.
---@param slot integer
---@return GearSaveInfo?
function GearGame.save_info(slot) end
