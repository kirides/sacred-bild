---@meta _
-- Sacred Gear: widgets, panels, regions and the layout (the second half of gear.ui).

-- ## Widgets and input

--: Widgets are immediate mode too: call them every frame with the current values; each draws itself, takes the mouse
--: over its rectangle and returns what the player did since the last frame. `id` names the widget within the mod (any
--: string or number, unique per widget). Positions are canvas coordinates (not `opts.window`).

--- Options every widget takes.
---@class GearWidgetOpts
---@field tooltip? string Shown after the cursor rests on the widget for 0.4 s.
---@field layer? GearLayer Default `'hud'`.
---@field enabled? boolean `false`: greyed out, no clicks.

---@class GearLabelOpts : GearWidgetOpts
---@field font? 'ui'|'game1'|'game2'|'game3'|'game4'|'game5'|'game6'|'game7'|integer
---@field size? number
---@field face? string
---@field bold? boolean
---@field italic? boolean
---@field height? number
---@field valign? GearTextVAlign
---@field color? GearColor

---@class GearButtonOpts : GearWidgetOpts
---@field image? integer A UI image beside the text.

---@class GearRadioGroupOpts : GearWidgetOpts
---@field horizontal? boolean Lay the options out in a row.
---@field spacing? number Between the options (default 18 vertical, 8 horizontal).
---@field tooltips? string[] One per option.

---@class GearSliderOpts : GearWidgetOpts
---@field step? number Default 1.
---@field format? string|false A `string.format` format for the number shown (default `'%g'`); false: no number.

---@class GearPanelOpts : GearWidgetOpts
---@field closable? boolean Default true: an X in the title bar.
---@field draggable? boolean Default true: the title bar drags it.
---@field resizable? boolean A corner grip resizes it.
---@field min_width? number Default 120.
---@field min_height? number Default 60.
---@field background? GearColor

--- What `gear.ui.region` reports.
---@class GearRegionState
---@field hover boolean The topmost region under the cursor (decided against the previous frame's regions).
---@field pressed boolean The left button went down on it since the last frame.
---@field released boolean
---@field clicked boolean Pressed and released over it.
---@field held boolean Pressed and not released yet.

--- A panel's state. When closed only `open` (false) is set.
---@class GearPanel
---@field open boolean
---@field key? string `mod_id:panel_id`, the layout key.
---@field x? number
---@field y? number
---@field w? number
---@field h? number
---@field content_x? number Where the content starts.
---@field content_y? number
---@field content_w? number
---@field content_h? number

---@class GearUi
local GearUi = {}

--- A rectangle that takes the mouse, for widgets of a mod's own. A left click (press and release) over a mod's region is
--- taken from the game: neither its windows nor the world see it. Regions on the world and hud layers take nothing while
--- the escape menu or a message box is up.
---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param h number
---@param opts? GearWidgetOpts
---@return GearRegionState
function GearUi.region(id, x, y, w, h, opts) end

--- Options of `tooltip_rect`.
---@class GearTooltipRectOpts
---@field x? number The point to keep clear (default: the cursor's hotspot).
---@field y? number
---@field bounds? number[] `{left, top, right, bottom}` the tooltip must stay inside (default: the screen); pass a panel's rectangle to keep it in that panel.
---@field cursor? boolean `false`: the point has no cursor sprite below and right of it, so only a small gap is kept (default true).

--- Where to put a `w` x `h` tooltip, or any popup beside the cursor, so that it never lies under the game's cursor
--- sprite (an arrow reaching about 30 x 38 layout units right and down of the hotspot) and stays inside the screen.
--- The default place is below the cursor's tip, to its right; when that leaves `bounds` it flips to below-left,
--- above-right, above-left, then beside the cursor, and as a last resort is clamped and pushed off the sprite. The
--- widgets' `tooltip` option already uses it. Returns the top-left corner.
---@param w number
---@param h number
---@param opts? GearTooltipRectOpts
---@return number x
---@return number y
function GearUi.tooltip_rect(w, h, opts) end

--- Text in the widgets' look (the UI font), with a tooltip if `opts.tooltip`.
---@param id string|integer
---@param text any
---@param x number
---@param y number
---@param opts? GearLabelOpts
---@return boolean drawn
function GearUi.label(id, text, x, y, opts) end

--- True when clicked.
---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param h number
---@param text any
---@param opts? GearButtonOpts
---@return boolean clicked
function GearUi.button(id, x, y, w, h, text, opts) end

---@param id string|integer
---@param x number
---@param y number
---@param text any
---@param checked boolean
---@param opts? GearWidgetOpts
---@return boolean changed
---@return boolean checked
function GearUi.checkbox(id, x, y, text, checked, opts) end

--- True when clicked.
---@param id string|integer
---@param x number
---@param y number
---@param text any
---@param selected boolean
---@param opts? GearWidgetOpts
---@return boolean clicked
function GearUi.radio(id, x, y, text, selected, opts) end

---@param id string|integer
---@param x number
---@param y number
---@param options string[]
---@param index integer The selected option (1-based).
---@param opts? GearRadioGroupOpts
---@return boolean changed
---@return integer index
function GearUi.radio_group(id, x, y, options, index, opts) end

--- The open list lies on the top layer; a click beside it closes it.
---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param options string[]
---@param index integer The selected option (1-based).
---@param opts? GearWidgetOpts
---@return boolean changed
---@return integer index
function GearUi.dropdown(id, x, y, w, options, index, opts) end

---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param min number
---@param max number
---@param value number
---@param opts? GearSliderOpts
---@return boolean changed True while dragged.
---@return number value
function GearUi.slider(id, x, y, w, min, max, value, opts) end

--- A window: its title bar drags it (the position is kept in the layout), its X closes it (`open` false). Draw the
--- content in the returned rectangle.
---@param id string|integer
---@param title any
---@param x number The default place (the layout's place wins).
---@param y number
---@param w number
---@param h number
---@param opts? GearPanelOpts
---@return GearPanel
function GearUi.panel(id, title, x, y, w, h, opts) end

--- Opens or closes a panel.
---@param id string|integer
---@param open boolean
function GearUi.show_panel(id, open) end

---@param id string|integer
---@return boolean
function GearUi.panel_open(id) end

-- ## Layout: persistent panels, movable game windows

--: Every panel has the id `mod_id:panel_id`. Where the player drags it (and how large they make it, with
--: `opts.resizable`) is kept in `mods\gear_layout.json` and restored at the next start. Places are stored in a virtual
--: 4096 x 4096 screen (the whole screen, whatever its resolution and aspect). The game's own windows are kept there
--: too, as `game:UI_WND_INVENTORY` and so on. Moving game windows needs SacredBild's UI frames.

--- A panel as `gear.ui.panels` lists it.
---@class GearPanelInfo
---@field key string
---@field mod string
---@field id string|integer
---@field title string
---@field x number
---@field y number
---@field w number
---@field h number
---@field resizable boolean
---@field min_width number
---@field min_height number
---@field layer GearLayer

---@class GearLayoutEntry
---@field x number In virtual units.
---@field y number
---@field w? number
---@field h? number

--- The layout edit mode flag, for every mod (panels get a highlighted frame).
---@return boolean
function GearUi.edit_mode() end

---@param on boolean
function GearUi.set_edit_mode(on) end

--- Whether the game's windows can be moved (needs SacredBild's UI frames).
---@return boolean
function GearUi.can_move_windows() end

--- The panels of every mod drawn this tick (those not drawn yet: as of the last tick).
---@return GearPanelInfo[]
function GearUi.panels() end

--- Puts any mod's panel at (x, y) of the canvas and, if it is resizable, makes it w x h (nil: the size stays); kept in
--- the layout, taken the next time its mod draws it.
---@param key string
---@param x number
---@param y number
---@param w? number
---@param h? number
---@return boolean found False for an unknown panel.
function GearUi.set_panel_rect(key, x, y, w, h) end

--- A panel back to the place and size its mod gives it.
---@param key string
---@return boolean found
function GearUi.reset_panel(key) end

--- Puts game window `name` with its top-left corner at (x, y) of the canvas and keeps it in the layout. Any in-game
--- window but those the game draws alone (world map, savegame, options, character export).
---@param name string
---@param x number
---@param y number
---@return boolean ok
function GearUi.set_window_position(name, x, y) end

--- A game window back to its own place.
---@param name string
---@return boolean found
function GearUi.reset_window(name) end

--- Forgets all places, or those whose key starts with `prefix` (e.g. `'game:'`).
---@param prefix? string
function GearUi.reset_layout(prefix) end

--- The stored places.
---@return table<string, GearLayoutEntry>
function GearUi.layout() end

--- From the canvas to the virtual 4096 x 4096 screen.
---@param x number
---@param y number
---@return number vx
---@return number vy
function GearUi.to_virtual(x, y) end

---@param vx number
---@param vy number
---@return number x
---@return number y
function GearUi.from_virtual(vx, vy) end
