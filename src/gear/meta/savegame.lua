---@meta _
-- Sacred Gear: data that belongs to the savegame being played (gear.savegame).

-- ## Per savegame data (gear.savegame)
--: Mods keep two kinds of data: what is the player's (settings, window places, a stash all characters share) and what belongs to
--: one character or game (progress, lifetime totals, a log). The second kind must not leak from one savegame into another, so
--: Gear keeps a working copy of it per mod in `saves/_current/` of the mod's folder and moves it with the game:
--:
--: - a save (the savegame window, the quicksave, the game's own auto save, `gear.game.save`) keeps the working copy for that
--:   savegame, in `saves/GAME05/` for `Save\GAME05.PAK` (first the event `savegame.saving`, so a mod can write what changed);
--: - loading a savegame (from the menus, the savegame window, `gear.game.load`) replaces the working copy with the one kept for
--:   it, or empties it if there is none (then the mod starts from its defaults); a network game's hero file
--:   (`Save\Hero03.pax`) has the scope `HERO03` and is kept when the game writes it;
--: - a new game empties the working copy; deleting a savegame (`gear.game.delete_save`) deletes what was kept for it.
--:
--: Before that, `savegame.leaving` `(scope, kind)` lets a mod write into the data kept for the game that ends. After a load or a new game the event `savegame.changed` `(kind, scope, number)` tells the mods to read their data again;
--: progress since the last save is gone, as the game's own is. The scope is the file's name (`GAME05`); when the game reads its
--: savegames from another folder than `Save` (ReBorn: `mods\reborn\save`) it has the folder in front: `mods_reborn_save-GAME05`.
--:
--: Most mods use `gear.store.game` (a JSON file with the usual handle) and never touch this API directly. Not for settings (use
--: `gear.settings`) and not for what all savegames share (use `gear.store.file`).
--:
--: Known limits: the load and new game commands are seen where the game runs them; a savegame copied or deleted outside the game
--: keeps (or loses) nothing here; in a network game the data follows the hero file, not the server.

-- path gear.savegame
--- The savegame being played.
---@class GearSavegame
local GearSavegame = {}

--- The savegame this game was loaded from or last saved to: `'GAME05'` (`Save\GAME05.PAK`), `'HERO03'` (a network hero file),
--- `<folder>-GAME05` when the game reads its savegames from another folder. nil in a new game that was not saved yet.
---@return string?
function GearSavegame.scope() end

--- How this game started: `'load'` (a savegame), `'hero'` (a network hero file), `'new'` (a new game); nil if the game's load
--- command was not seen (the game was running before Gear, or the command was not found: the data is then global as before).
---@return ('load'|'hero'|'new')?
function GearSavegame.kind() end

--- The scope name of savegame `slot` (`'GAME05'`).
---@param slot integer
---@return string
function GearSavegame.scope_of(slot) end

--- A file of this mod's data of the running game (`.json` or `.txt`, a name of letters, digits, `_`, `-` and `.`). Using this API
--- makes Gear keep the mod's data per savegame.
---@param name string
---@return string? text
---@return string? error
function GearSavegame.read(name) end

--- Writes a file of this mod's data of the running game (atomic, at most 8 MB).
---@param name string
---@param text string
---@return boolean? ok
---@return string? error
function GearSavegame.write(name, text) end

--- Writes a file of the data kept for savegame `scope` (not the running game's own). For what a mod learns after the savegame was
--- written and must attribute to it, such as the time played when the game ends: write it in the handler of `savegame.leaving`.
---@param scope string
---@param name string
---@param text string
---@return boolean? ok
---@return string? error
function GearSavegame.write_saved(scope, name, text) end

--- A file of the data kept for another savegame (for a list that shows what each savegame holds).
---@param scope string
---@param name string
---@return string? text
---@return string? error
function GearSavegame.read_saved(scope, name) end
