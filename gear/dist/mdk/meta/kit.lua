---@meta _
-- Sacred Gear: the core widgets of RmlUi screens (gear.kit).

-- ## Widgets for RmlUi screens (gear.kit)
--: `gear.kit` builds the markup of the usual pieces of a screen (a window, tabs, buttons, check boxes, sliders, rows, bars, lists, a
--: confirm dialog) from the class names that every look understands: the plain base style of Sacred Gear and the theme mods
--: (**sacred-theme**) style the same classes. A screen made of the kit therefore runs and reads well with no theme, and looks like the game
--: with one; every button, link and input is a stop for the controller. The functions return strings of RML to join and give to
--: `gear.ui.rml.load_string` (see `page`); text arguments are escaped, `body` and `control` are markup. The library is shipped in the
--: DLL (`gear/lua/kit.lua`) and runs once per mod, the first time the mod uses `gear.kit`.

--- The options of `window`.
---@class GearKitWindow
---@field title? string The title bar's text (no title bar without it).
---@field id? string
---@field x? number Where it is placed (layout units; makes it `position: absolute`).
---@field y? number
---@field width? number
---@field height? number
---@field panel? string A window the layout editor can move (`data-panel`; the place is kept under `<mod id>:<panel>`).
---@field resizable? boolean The editor sizes it too (`data-resizable`).
---@field close? string The id of a close button in the title bar.
---@field class? string Extra classes.
---@field body? string The content (markup).

---@class GearKitTab
---@field id string
---@field label string

---@class GearKitTabs
---@field id? string The prefix of the tab elements' ids (`<id>-<tab id>`, default `tab`).
---@field tabs GearKitTab[]
---@field active? string The id of the open tab.

---@class GearKitButtonOpts
---@field primary? boolean The bright gold edge.
---@field danger? boolean
---@field class? string
---@field disabled? boolean

---@class GearKitListItem
---@field id string
---@field name string
---@field sub? string
---@field selected? boolean

---@class GearKitConfirm
---@field title? string
---@field text? string
---@field yes? string Default `OK`.
---@field no? string Default `Cancel`.
---@field danger? boolean The yes button is the red one.
---@field on_result? fun(yes: boolean) Runs once.

--- A modal question; call `draw` from the mod's draw handler while `open()` is true.
---@class GearKitDialog
local GearKitDialog = {}
--- Whether the answer is still to come.
---@return boolean
function GearKitDialog:open() end
--- Answers no.
function GearKitDialog:cancel() end
--- Draws it (the game and the other overlays take no mouse and no pad meanwhile).
function GearKitDialog:draw() end

-- path gear.kit
--- The widgets of this mod's screens.
---@class GearKit
local GearKit = {}

--- Text safe in RML (also in an attribute).
---@param text any
---@return string
function GearKit.esc(text) end

--- A window frame with an optional title bar and a body.
---@param o GearKitWindow
---@return string markup
function GearKit.window(o) end

--- A tab header bar; the tabs are links (`doc:on('#tab-a', 'click', fn)`).
---@param o GearKitTabs
---@return string markup
function GearKit.tabs(o) end

--- A game button.
---@param id string
---@param label string
---@param o? GearKitButtonOpts
---@return string markup
function GearKit.button(id, label, o) end

--- The button of a drop-down: the game's arrow caps (images 267 / 270, middle 281) with the label between; the list it opens is the mod's (`list`).
---@param id string
---@param label string
---@param o? {tip?: string, disabled?: boolean}
---@return string markup
function GearKit.dropdown(id, label, o) end

--- A row with the label and the game's check orb (an `input`: listen for `change`).
---@param id string
---@param label string
---@param checked boolean
---@param o? {disabled?: boolean, tip?: string}
---@return string markup
function GearKit.check(id, label, checked, o) end

--- The game's slider (`change` has `params.value`).
---@param id string
---@param min number
---@param max number
---@param step number
---@param value number
---@param o? {class?: string}
---@return string markup
function GearKit.slider(id, min, max, step, value, o) end

--- A grey label at the left and the control (markup) at the right.
---@param label string
---@param control string
---@param o? {tip?: string, class?: string}
---@return string markup
function GearKit.row(label, control, o) end

--- A section title.
---@param text string
---@return string markup
function GearKit.heading(text) end

--- A paragraph; `class` may be `muted`, `accent`, `good` or `bad`.
---@param text string
---@param o? {class?: string}
---@return string markup
function GearKit.text(text, o) end

--- A bar (200 layout units wide unless `width`) filled to a fraction (0 to 1); class `mana` or `xp` for the other colours.
---@param fraction number
---@param o? {id?: string, class?: string, width?: number}
---@return string markup
function GearKit.progress(fraction, o) end

--- A list of items; each is a button (a pad stop).
---@param items GearKitListItem[]
---@param o? {class?: string}
---@return string markup
function GearKit.list(items, o) end

--- Buttons side by side at the right (the markup of `button`).
---@param ... string
---@return string markup
function GearKit.buttons(...) end

--- A whole document around the markup, for `gear.ui.rml.load_string`.
---@param markup string
---@param o? {head?: string}
---@return string
function GearKit.page(markup, o) end

--- A modal question in the middle of the screen.
---@param o GearKitConfirm
---@return GearKitDialog
function GearKit.confirm(o) end
