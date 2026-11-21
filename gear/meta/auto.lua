---@meta _
-- Sacred Gear: automation (gear.auto).

-- ## Automation (gear.auto)

---@alias GearAutoKey 'back'|'tab'|'enter'|'shift'|'control'|'alt'|'pause'|'escape'|'space'|'left'|'up'|'right'|'down'|'delete'|'f1'|'f2'|'f3'|'f4'|'f5'|'f6'|'f7'|'f8'|'f9'|'f10'|'f11'|'f12'|'f13'|'f14'|'f15'|'f16'|'f17'|'f18'|'f19'|'f20'|'f21'|'f22'|'f23'|'f24'|'a'|'b'|'c'|'d'|'e'|'f'|'g'|'h'|'i'|'j'|'k'|'l'|'m'|'n'|'o'|'p'|'q'|'r'|'s'|'t'|'u'|'v'|'w'|'x'|'y'|'z'|'0'|'1'|'2'|'3'|'4'|'5'|'6'|'7'|'8'|'9'|integer
---@alias GearMouseButton 'lbutton'|'rbutton'|'mbutton'|'xbutton1'|'xbutton2'

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

--- The controller's LB / RB without one: posts `pages` page steps (-1 previous, +1 next), read by `ui.page_pressed`.
---@param pages integer
function GearAuto.pad_page(pages) end

--- The D-pad's up (-1) / down (+1) steps in a scroll area (a list marked `data-nav-scroll`, or a `ui.scroll` area), as the controller posts them: they
--- act at the game cursor (`gear.auto.move`).
---@param steps integer
function GearAuto.pad_scroll(steps) end

--- The Select / "-" button without a controller: one press, read by `ui.select_pressed`.
function GearAuto.pad_select() end

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

--- Where a mod's widget was drawn in the last tick (UI layout coordinates, as `ui_click` takes them), found by the id it was
--- given (`gear.ui.button('save', ...)`, a table cell's `id`); clipped to its scroll area. Nil when it was not drawn.
---@param id string|integer
---@param mod_id? string The mod that drew it (default: this mod).
---@return number? x
---@return number? y
---@return number? w
---@return number? h
function GearAuto.widget(id, mod_id) end

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

--- The number the game's own "first free savegame number" (the function a new savegame without a number asks) answers now:
--- the engine's function itself, called through Gear's replacement, so it is not limited to 509 savegames. nil if it was not found.
---@return integer?
function GearAuto.engine_free_slot() end

--- Tests the stubs of the constant site hooks on two functions built in memory (`fmul dword [c]` and `fld dword [c]`, with the
--- sites `selftest.fmul` and `selftest.fld` of the event `combat.constant`); a worker thread calls them when Lua is idle.
---@return boolean started false if a test is running.
function GearAuto.site_hook_selftest_start() end

--- The result of the test: `fmul` and `fld` are st0 after the instruction (the answer of the mods, else vanilla 0.2 and 0.2 times
--- 1.0), `registers_kept` whether eax, ecx and edx survived the stub. The `ability_*` fields test the `creature.ability` stubs on three
--- functions built in memory (a store of 100, an add of 1000 and the computed block; a fake creature of type 77): the stored id, the
--- amount added and edx after the block (vanilla 100, 1000 and 50 without an answer), `ability_kept` whether the registers survived.
--- `damage` is the result of the `damage` stub on a function built in memory (12.4 rounds to 12 without an answer).
--- The `effect_*` fields test the `effect.add` stub on a function built in memory with a fake effect list (the sum 3.5 for entry 0, the
--- creature's counter 4): the exit taken (1 engine path, 2 next entry, 3 leave), both entries' values times 100 and the counter.
--- `bonus_int` / `bonus_float` are what the `bonus.constant` stubs push on functions built in memory (`push 5` and `push 3.5`).
--- `slot_byte` is 1 when the `skillslot.select` stub kept the displaced instruction working on a function built in memory.
--- `branch_taken` is 2 when the `combat.branch` stub took the jump of a function built in memory (1: fell through).
--- `branch2_path` / `branch2_eax`: the same for a `jcc rel8` whose next instruction (`add eax, -2`, eax = 0x28) is displaced with it.
--- nil until it is done.
---@return {fmul: number, fld: number, registers_kept: boolean, ability_store: integer, ability_add: integer, ability_edx: integer, ability_kept: boolean, damage: integer, effect_exit: integer, effect_entry0: integer, effect_entry1: integer, effect_counter: integer, bonus_int: integer, bonus_float: number, slot_byte: integer, branch_taken: integer, branch2_path: integer, branch2_eax: integer}?
function GearAuto.site_hook_selftest_result() end

--- The raw bytes of an object as lower-case hex (for self tests and reverse engineering), `size` 1 to 4096 bytes from `offset`.
---@param id integer
---@param offset integer
---@param size integer
---@return string? hex
---@return string? error
function GearAuto.object_bytes(id, offset, size) end

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

--- Sets or clears the engine's network-game flag (the byte the engine's own object and inventory code test), to test that
--- the guards (`world.can_change`, inventory changes, `spawn`) refuse in a network game. Set it, test and clear it in the
--- same tick: nothing else runs in between.
---@param on boolean
---@return boolean ok
function GearAuto.network_flag(on) end

--- Sets the flags `gear.mp.mode()` reads (`'single'`, `'lan'`, `'open'`, `'closed'`; the network-game flag too), to test what depends
--- on the kind of game without one. Read the old mode first and put it back in the same tick: nothing else runs in between.
---@param mode 'single'|'lan'|'open'|'closed'|'internet' `'internet'`: the Internet menu was used and nothing is proven (reads as `'closed'`)
---@return boolean ok
function GearAuto.network_mode(mode) end

--- Sets or clears "the game server delivered the hero" (message 0x132), the proof of Closed Net that `gear.mp.mode()` reads;
--- set, an `'open'` game reads as `'closed'`.
---@param on boolean
function GearAuto.hero_from_server(on) end

--- Adds a line to the game's message list as the game itself does (through Gear's hook: `chat.line` gets it, and a
--- replaced overlay keeps it off the screen). In game only. For self tests of `gear.chat`.
---@param text string UTF-8, with the game's backslash codes if wanted.
---@param channel? integer 1 (default) to 4, or 0x10.
---@param color? integer ARGB; 0 (default): the channel's colour.
---@return boolean ok
function GearAuto.chat_add(text, channel, color) end

--- Sends a chat message on the network as the game's chat does (a network game only): `'private'` to `target` (a slot),
--- `'team'` or `'global'`. For self tests of `gear.chat`: a private message to the own slot goes out to the game
--- server and comes back like another player's.
---@param kind 'private'|'team'|'global'
---@param target integer The slot for `'private'` (0 otherwise).
---@param text string At most 127 characters.
---@return boolean ok
function GearAuto.chat_send(kind, target, text) end

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
