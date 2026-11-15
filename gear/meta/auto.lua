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

---@param key GearAutoKey
function GearAuto.key_down(key) end

---@param key GearAutoKey
function GearAuto.key_up(key) end

--- Releases everything held.
function GearAuto.release() end

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

--- Writes a file into `GearAutomation\<instance>`.
---@param name string
---@param text string
---@return boolean? ok
function GearAuto.write(name, text) end

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
