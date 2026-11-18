---@meta _
-- Sacred Gear: toasts, short messages on the screen (gear.toast).

-- ## Toasts (gear.toast)
--: A toast is a short message (a title and a few lines) that shows at the top of the screen, stacks under the ones before it and
--: fades after a few seconds: an unlocked achievement, a finished bounty. The stack is a bare panel of the mod (`'toasts'`, layout
--: key `<mod id>:toasts`): the layout editor moves it, and shows a sample while it is on; the mouse and the controller go on to
--: the game. A toast's age counts in-game frames. The library is shipped in the DLL (`gear/lua/toast.lua`) and runs once per mod,
--: the first time the mod uses `gear.toast`.

--- A toast given as a table to `show`.
---@class GearToastSpec
---@field title string
---@field lines? string[]
---@field color? integer The accent bar's colour (0xAARRGGBB).

--- The options of `options`.
---@class GearToastOpts
---@field seconds? number|fun(): number How long a toast stays (default 6); a function is asked every frame (e.g. a setting).
---@field max? integer How many toasts at once (default 4); the oldest goes first.
---@field width? number The width of a toast in layout units (default 360).
---@field when? fun(state: GearUiState): boolean Whether toasts are drawn now (default: `state.mode == 'game'`).

-- path gear.toast
--- This mod's toasts.
---@class GearToast
local GearToast = {}

--- Shows a toast: a title and lines (nil and `''` are left out), or a `GearToastSpec`.
---@param title string|GearToastSpec
---@param ... string?
function GearToast.show(title, ...) end

--- Changes this mod's toast options (each one is kept until changed).
---@param opts GearToastOpts
function GearToast.options(opts) end

--- Removes this mod's toasts.
function GearToast.clear() end

--- How many of this mod's toasts are on the screen.
---@return integer
function GearToast.count() end
