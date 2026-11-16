---@meta _
-- Sacred Gear: automation (gear.auto).

-- ## Automation (gear.auto)

---@alias GearAutoKey 'back'|'tab'|'enter'|'shift'|'control'|'alt'|'pause'|'escape'|'space'|'left'|'up'|'right'|'down'|'delete'|'f1'|'f2'|'f3'|'f4'|'f5'|'f6'|'f7'|'f8'|'f9'|'f10'|'f11'|'f12'|'a'|'b'|'c'|'d'|'e'|'f'|'g'|'h'|'i'|'j'|'k'|'l'|'m'|'n'|'o'|'p'|'q'|'r'|'s'|'t'|'u'|'v'|'w'|'x'|'y'|'z'|'0'|'1'|'2'|'3'|'4'|'5'|'6'|'7'|'8'|'9'|integer
---@alias GearMouseButton 'lbutton'|'rbutton'|'mbutton'

--- The UI manager's state.
---@class GearAutoState
---@field ui boolean
---@field menus boolean
---@field in_game boolean
---@field cinematic boolean
---@field help boolean
---@field dialog boolean
---@field window boolean A game window exists.
---@field width integer The client size.
---@field height integer
---@field mouse_x number The game's cursor in UI layout coordinates.
---@field mouse_y number

--- One visible UI node: menus, in-game windows, the message box and their visible children.
---@class GearAutoUiNode
---@field group string
---@field index integer
---@field parent integer Index into the list, 0 top level.
---@field vtable integer
---@field flags integer
---@field x number In UI layout coordinates.
---@field y number
---@field width number
---@field height number

--- For self tests and scripted runs without a player. `gear.auto` exists only when the game starts with `-gear-auto` (or
--- `SACREDGEAR_AUTO=1`) and the mod's manifest says `automation = true`. The functions marked "in a task" wait, so
--- they work only inside `gear.task`.
-- path gear.auto
---@class GearAuto
---@field VK table<string, integer> Key names to virtual key codes.
---@field instance string The run's name (`-gear-instance`).
local GearAuto = {}

---@return GearAutoState
function GearAuto.state() end

--- The visible UI.
---@return GearAutoUiNode[]
function GearAuto.ui() end

--- Moves the game's cursor; client pixels.
---@param x number
---@param y number
function GearAuto.move(x, y) end

---@param button? GearMouseButton Default `'lbutton'`.
function GearAuto.down(button) end

---@param button? GearMouseButton Default `'lbutton'`.
function GearAuto.up(button) end

--- The mouse wheel at the cursor, as `WM_MOUSEWHEEL`: + away from the player (scrolls up), - towards.
---@param notches? number Default 1.
function GearAuto.wheel(notches) end

---@param key GearAutoKey
function GearAuto.key_down(key) end

---@param key GearAutoKey
function GearAuto.key_up(key) end

--- Releases everything held.
function GearAuto.release() end

--- The controller without one: posts pad events as SacredBild's controller does (D-pad steps on a slider, B presses,
--- mod menu button presses).
---@param nudge? integer
---@param back? integer
---@param menu? integer
function GearAuto.pad(nudge, back, menu) end

--- The stops SacredBild holds for the controller: their count (-1 stale, -2 no SacredBild) and flags (1 modal, 2 a
--- mod window is open).
---@return integer count
---@return integer flags
function GearAuto.nav() end

--- In a task: moves, presses and releases with short pauses, as a player would. Client pixels.
---@param x number
---@param y number
---@param button? GearMouseButton
function GearAuto.click(x, y, button) end

--- In a task: presses and releases a key.
---@param key GearAutoKey
function GearAuto.press(key) end

--- In a task: types text, one character at a time.
---@param text string
function GearAuto.type(text) end

--- In a task: the client pixel for the UI layout point (x, y); the mapping is measured once by moving the cursor.
---@param x number
---@param y number
---@return integer? px
---@return integer|string? py The pixel, or the error message when `px` is nil.
function GearAuto.ui_to_pixels(x, y) end

--- In a task: clicks a point in UI layout coordinates, or a node's center.
---@param x number|GearAutoUiNode
---@param y? number|GearMouseButton With a node: the button.
---@param button? GearMouseButton
---@return boolean? ok
---@return string? error
function GearAuto.ui_click(x, y, button) end

--- Loads savegame `slot` from the menus.
---@param slot integer
---@return boolean? ok
function GearAuto.load_save(slot) end

--- Saves a screenshot (`.bmp`) of the game's back buffer into `GearAutomation\<instance>`. Name: letters, digits, `_ - .`.
---@param name string
---@return boolean? ok
function GearAuto.screenshot(name) end

--- Starts the engine's own level function on a worker thread (the first of the `level.factor` hooks): the call goes through the
--- hook, so a mod that handles `level.factor` answers it. For tests.
---@param level integer
---@return boolean started false if a call is running or the function was not found.
function GearAuto.engine_level_factor_start(level) end

--- The integer the call started by `engine_level_factor_start` returned; nil until it is done.
---@return integer?
function GearAuto.engine_level_factor_result() end

--- Tests the stubs of the constant site hooks on two functions built in memory (`fmul dword [c]` and `fld dword [c]`, with the
--- sites `selftest.fmul` and `selftest.fld` of the event `combat.constant`); a worker thread calls them when Lua is idle.
---@return boolean started false if a test is running.
function GearAuto.site_hook_selftest_start() end

--- The result of the test: `fmul` and `fld` are st0 after the instruction (the answer of the mods, else vanilla 0.2 and 0.2 times
--- 1.0), `registers_kept` whether eax, ecx and edx survived the stub. The `ability_*` fields test the `creature.ability` stubs on three
--- functions built in memory (a store of 100, an add of 1000 and the computed block; a fake creature of type 77): the stored id, the
--- amount added and edx after the block (vanilla 100, 1000 and 50 without an answer), `ability_kept` whether the registers survived.
--- `damage` is the result of the `damage` stub on a function built in memory (12.4 rounds to 12 without an answer).
--- nil until it is done.
---@return {fmul: number, fld: number, registers_kept: boolean, ability_store: integer, ability_add: integer, ability_edx: integer, ability_kept: boolean, damage: integer}?
function GearAuto.site_hook_selftest_result() end

--- Writes a file into `GearAutomation\<instance>`.
---@param name string
---@param text string
---@return boolean? ok
function GearAuto.write(name, text) end

---Starts GameServer.exe the way the game does for a hosted LAN game (with Sacred Gear injected when mods overlay files),
---lets it run `seconds` (default 8) and ends it. Its logs are in the game folder (SacredGear-server.log, SacredBild-server.log).
---@param seconds? number
---@return integer pid 0 if it did not start
function GearAuto.start_gameserver(seconds) end

--- Shows or hides an in-game window by its control name (`UI_WND_CHEST` ...) through the window's own `show`, without the
--- events that normally open it (the chest window opens when the hero uses a chest). For self tests; false if there is
--- no such window.
---@param name string
---@param shown boolean
---@return boolean ok
function GearAuto.show_window(name, shown) end

--- Ends the game at once with that exit code.
---@param code? integer Default 0.
function GearAuto.quit(code) end

--- In a task: waits until the main menu is up.
---@param timeout? number Default 120.
---@return boolean up
function GearAuto.wait_for_menu(timeout) end

--- In a task: waits until a game with a hero runs.
---@param timeout? number Default 180.
---@return boolean running
function GearAuto.wait_for_game(timeout) end
