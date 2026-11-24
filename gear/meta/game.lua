---@meta _
-- Sacred Gear: the game itself, saving.

-- ## Saving (gear.game)

-- path gear.game
--- Saving the game through the engine's own save request, the one the savegame window sends: the file is
--- `Save\GAME<slot>.PAK` (slot 0 is the quicksave; the game lists the savegames sorted by file name). Single player
--- only. A save is a request: the engine writes the file a moment later on its render thread, and the result comes as
--- the events `game.saved` and `game.save_failed`. Saves nobody asked this API for (the quicksave key, the savegame window, the game's own Auto-Save) come as `game.saved_other`. Slots 1 to 99 show as `GAME01` to `GAME99`; a mod that saves on
--- its own should keep to slots the player does not use (the game's savegame window picks the first free number
--- for manual saves).
---@class GearGame
local GearGame = {}

--- The difficulty level of the running game: 0 (bronze) to 4 (niobium); nil if the game is not running.
---@return integer?
function GearGame.difficulty() end

--- One service symbol of the world map (what the game draws on it for a trader).
---@class GearMapMarker
---@field x integer Tile x (world units / `gear.world.TILE`).
---@field y integer Tile y.
---@field kind integer 0 merchant, 1 blacksmith, 2 combo master, 3 horse trader.
---@field region integer The region id the game files it under.

--- The service symbols of the world map, read from the game's `bin\merc.bin` (once, then kept). nil if the file is not there.
---@return GearMapMarker[]?
function GearGame.map_markers() end

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
---@field window? boolean Save as the open savegame window's own Save button does, for a mod that replaces the window (`gear.ui.replace_window`): only while that window is open in game; the picture is the one the game took when it opened, and the window closes once the engine took the request.

--- Asks the engine to save into slot `slot` (0 to 32766: `Save\GAME05.PAK`, `Save\GAME500.PAK`, ... `Save\GAME32766.PAK`; the game itself stopped at 509). The engine refuses while a trading, chest or similar window
--- is open. Returns true when the request is accepted; the outcome is the event `game.saved` or `game.save_failed`.
---@param slot integer
---@param name? string The savegame's display name, UTF-8, at most 60 characters; the game keeps Windows-1252 (umlauts and the like), anything else becomes `?`.
---@param opts? GearSaveOpts
---@return boolean? ok
---@return string? error
function GearGame.save(slot, name, opts) end

--- Whether a save (or a load) is queued or being written.
---@return boolean
function GearGame.save_pending() end

--- A value of the game's own settings (`Settings.cfg` in the game folder), read only: numbers as numbers (`0` is
--- truthy in Lua, compare it: `gear.game.config('AUTOSAVE') == 1`), everything else as text. The key is not case
--- sensitive. nil if the file has no such key. This is what the game was set to, not a live view: the options window
--- writes the file when the player confirms it, and many settings are read only at start. Useful for warnings about
--- settings that clash with a mod (`AUTOSAVE`: the game's own autosave at script points).
---@param key string
---@return string|number|nil
function GearGame.config(key) end

--- Every value of `Settings.cfg` (see `config`), keyed by the upper case name.
---@return table<string, string|number>
function GearGame.config_all() end

--- The file of a slot.
---@class GearSaveInfo
---@field exists boolean
---@field size number Bytes.
---@field mtime number Last write, unix seconds.

--- The savegame file of `slot`: nil for a slot outside 0..32766, else a table whose `exists` says if the file is there.
---@param slot integer
---@return GearSaveInfo?
function GearGame.save_info(slot) end

--- The first slot from 1 up whose savegame file does not exist: the number the game's own *new savegame* picks (its
--- search goes beyond 509 now). nil if every number up to 32766 is taken.
---@return integer?
function GearGame.free_slot() end

--- Slots this mod writes to itself (the autosave mod's rotation): `free_slot` and the game's own *new savegame* skip them
--- even while their files do not exist yet, so a manual save does not take one a mod is about to use. At most 64
--- numbers, each 1 to 32766; replaces this mod's earlier list (call it with `{}` to give them back).
---@param slots integer[]
function GearGame.reserve_slots(slots) end

--- A savegame as `saves` lists it: what the file says about itself.
---@class GearSavegame
---@field slot integer The number in its file name (`GAME05.PAK`: 5); 0 is the quicksave.
---@field file string The file name, e.g. `GAME05.PAK`.
---@field name string The savegame's name (UTF-8; may be empty, the game writes `QUICKSAVE` or its "empty" text).
---@field hero? string The hero's name (UTF-8); nil if the file does not say.
---@field class? GearClassName The hero's class; nil if unknown.
---@field class_id integer The hero's class number (1 seraphim .. 9 daemon), 0 if unknown.
---@field level integer The hero's level, 0 if unknown.
---@field version integer The savegame format (0x1B for Sacred Gold).
---@field compatible boolean Whether this game can load it (`version` is 0x1B).
---@field time number When it was saved (unix seconds, from the time the game wrote into it); 0 in old savegames.
---@field mtime number The file's last write (unix seconds).
---@field size number Bytes.
---@field world_date string The game world's date and time as the game shows it.
---@field picture boolean Whether it has a picture (`save_picture`).

--- Every savegame in the game's `Save` folder (or the folder a mod's overlay puts it in), sorted by file name like the
--- game's own list. Each file is read for its header: call it when a screen opens, not every frame.
---@return GearSavegame[]
function GearGame.saves() end

--- The savegame's picture (128 x 96, what the game captured when it was saved) as an id for `gear.ui.image`; made on
--- the first call and kept while the file stays the same. nil if it has none.
---@param slot integer
---@return integer? picture
function GearGame.save_picture(slot) end

--- Loads a savegame, as the savegame window's Load does: in a single player game or from the menus (the open savegame
--- window closes). Refuses a savegame of another version.
---@param slot integer
---@return boolean? ok
---@return string? error
function GearGame.load(slot) end

--- Deletes a savegame's file (the game's savegame windows read their lists again).
---@param slot integer
---@return boolean? ok
---@return string? error
function GearGame.delete_save(slot) end

-- ## Clipboard (gear.clipboard)

-- path gear.clipboard
--- The Windows clipboard, for a mod that shows something the player wants to paste elsewhere (a server admin's config line):
--- text only, Unicode.
---@class GearClipboard
local GearClipboard = {}

--- Puts `text` (UTF-8, at most 1 MB) on the clipboard, replacing what was there.
---@param text string
---@return boolean? ok
---@return string? error `'the clipboard is not available'` (another program holds it) or the size limit.
function GearClipboard.set(text) end

--- The text on the clipboard, nil when there is none (or it is not text).
---@return string?
function GearClipboard.get() end
