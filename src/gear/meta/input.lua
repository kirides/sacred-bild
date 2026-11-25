---@meta _
-- Sacred Gear: the game's own keys, remapped.

-- ## Remapping the game's keys (gear.input)
--: The game's keys are fixed: I opens the inventory, 6 selects combat art 1, Space drinks the healing potion. A mod
--: that lets the player choose other keys (or mouse buttons) for those actions gives Sacred Gear rules: "this input is
--: that game key". Sacred Gear applies them to the messages the game reads, before any of its handlers, so the game
--: reacts to the new key as if the player had pressed its own, and never sees the original. A rule without target makes
--: an input do nothing (the original key of an action that was moved elsewhere). Rules apply only while the game has
--: the keyboard: in game, with no message box, escape menu, savegame, options or character screen, and no chat line
--: open; elsewhere every key is the game's own. Inputs are keys (the names of `gear.ui.key_pressed`, with `f13` to
--: `f24` for the extra keys of mice and keyboards) and the mouse buttons `mouse3` (middle), `mouse4` and `mouse5`
--: (the side buttons), alone or with `ctrl+`, `shift+` and `alt+`; the modifiers named must be the only ones held.
--: A modifier alone is a binding too (`alt`), for the keys the game polls. Mouse buttons 1 and 2 and Esc cannot be rebound. The declared keys of `mod.lua` take the same names, so the mod
--: manager's rebinding screen (F11, with the controller's key picker) offers them.

---@class GearInputRule
---@field from string The input: `'x'`, `'f13'`, `'mouse4'`, `'shift+z'`.
---@field to? string The game's key it becomes (a key name without modifiers); not with `block`. `ctrl`, `shift`, `alt`, `add` and `subtract` are keys the game polls (Alt shows names, Ctrl attacks in place, numpad +/- zoom): its poll sees the input held instead, and the input's messages go nowhere.
---@field release? string Instead of the others: the game's poll of this polled key (`ctrl`, `shift`, `alt`, `add`, `subtract`) sees it released (its own key of an action moved elsewhere).
---@field block? boolean The input does nothing in game (with any modifiers held); the original key of a moved action.
---@field id? string Reports the press and release of this input to the handler of `gear.input.on`.

-- path gear.input
---@class GearInput
local GearInput = {}

--- Sets this mod's rules (replacing its earlier ones; `{}` removes them). A rule `{from, to}` for a key the game
--- already has under that name (`{from = '6', to = '6'}`) changes nothing; it marks the key as taken over by the mod,
--- so the mod manager does not warn that the game uses it. An exact rule for an input goes before a `block` rule of the
--- same key.
---@param rules GearInputRule[]
function GearInput.remap(rules) end

--- Calls `fn(id, down, repeating)` when the input of one of this mod's rules with an `id` is pressed or released
--- (`repeating`: a held key's auto repeat). The game has the key it was turned into at that time.
---@param fn fun(id: string, down: boolean, repeating: boolean)
---@return table handle
function GearInput.on(fn) end

--- Removes a handler of `on`.
---@param handle table
function GearInput.off(handle) end

--- Whether the rules apply now, and whether the chat line is open.
---@return boolean applies
---@return boolean typing
function GearInput.context() end

--- Rebinding screens: remembers which inputs are down, so `capture` returns the first one pressed after it. Call
--- `gear.ui.capture_keys(true)` as well, so the pressed key does not reach the game.
function GearInput.capture_start() end

--- The binding (`'ctrl+mouse4'`) of the first key or mouse button pressed since `capture_start`, or nil.
---@return string?
function GearInput.capture() end

--- The canonical text of a binding (`'Ctrl+Z'` gives `'ctrl+z'`; `''` stays), or nil if it is not one.
---@param text string
---@return string?
function GearInput.normalize(text) end
