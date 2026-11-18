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
---@field nav? boolean Default true (regions and every widget): a stop of the controller's D-pad. `false` for rectangles that only take the mouse (backgrounds, blockers).

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

---@class GearDropdownOpts : GearWidgetOpts
---@field max_rows? integer The open list shows at most this many entries and scrolls (default 12).

---@class GearScrollOpts
---@field step? number One line: Up / Down, an arrow button and the D-pad move it, a wheel notch three (default 20).
---@field layer? GearLayer Default `'hud'`.
---@field nav? boolean `false`: the area is no stop of the controller's D-pad.

--- What `scroll_begin` returns: the viewport (without the scrollbar) and where the content starts.
---@class GearScrollArea
---@field x number
---@field y number
---@field w number The viewport's width (the area's, less the scrollbar when it shows).
---@field h number
---@field offset number How far the content is scrolled (0 .. max).
---@field max number
---@field content_x number Where the content's top-left lies: draw at `content_y + its own y`.
---@field content_y number

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
---@field back? boolean Default true: the controller's B closes the panel while it is the topmost closable one; `false`: the mod handles B itself (`ui.back_pressed`).
---@field bare? boolean No window: no background, frame, title bar, close button or resize grip, and no region, so the mouse and clicks go on to the game; the mod draws in the whole rectangle (`content_*` is the rect). It is still a panel for the layout: the layout editor moves it (and resizes it with `resizable`) and `set_panel_rect` works; the layout editor's mode draws its frame. For a HUD element. `closable` and `draggable` do not apply.
---@field passive? boolean A closable panel that should not take the controller (no cursor, B not caught): a HUD-like panel.

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

--- The open list is drawn after every draw handler, over all panels and text, on the top layer, and takes the mouse
--- first; it shows at most `opts.max_rows` (12) entries and scrolls (wheel, scrollbar, keys, D-pad), the selected one
--- in view when it opens. A click beside it closes it.
---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param options string[]
---@param index integer The selected option (1-based).
---@param opts? GearDropdownOpts
---@return boolean changed
---@return integer index
function GearUi.dropdown(id, x, y, w, options, index, opts) end

--- Starts a vertically scrollable area: until `scroll_end`, what is drawn is clipped to its viewport (rects, images,
--- the UI font's text; the game's fonts and item models only when wholly inside) and regions are cut to it, so a
--- widget scrolled out of sight takes no click. Draw the content at `content_y + its own y`. The wheel scrolls the
--- area under the cursor (3 lines a notch), Up / Down / PageUp / PageDown / Home / End the one under the cursor or
--- clicked last, the controller's D-pad (SacredBild) the one its cursor is in, once no widget lies further that way
--- inside it, and the right stick as a wheel. The offset is kept per id for the session. Areas nest.
---@param id string|integer
---@param x number
---@param y number
---@param w number
---@param h number
---@param opts? GearScrollOpts
---@return GearScrollArea area
function GearUi.scroll_begin(id, x, y, w, h, opts) end

--- Ends the innermost scroll area and draws its scrollbar (arrows, track: a click pages, thumb: drag it) when the
--- content is taller than the viewport.
---@param content_height? number The content's height (default: the last one given).
---@return number offset
---@return number max
function GearUi.scroll_end(content_height) end

--- Sets a scroll area's offset (clamped when it is drawn next).
---@param id string|integer
---@param offset number
function GearUi.scroll_to(id, offset) end

--- A scroll area's offset and its largest offset (as of its last frame); 0, 0 for an unknown id.
---@param id string|integer
---@return number offset
---@return number max
function GearUi.scroll_offset(id) end

--- The mouse wheel's notches over mod UI this tick (+ away from the player, as Windows counts them; 0: none; a
--- notch is 1), and whether a scroll area already took them. Over a mod's widget the game does not get the wheel.
---@return number notches
---@return boolean taken
function GearUi.wheel() end

--- The text broken into lines that fit `width` (at spaces; a word longer than the width gets a line of its own;
--- `\n` breaks too), in the font of `opts` (as `text` takes it), and the lines' total height. Cached per text, width
--- and font.
---@param text string
---@param width number
---@param opts? GearTextOpts
---@return string[] lines
---@return number height
function GearUi.text_wrap(text, width, opts) end

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

--- The controller: while on, the mod's screen has the pad alone. Only its widgets are D-pad stops (the game's controls
--- and other mods' are not), the cursor starts on its first widget, and B is `back_pressed()`. Switch it off when the
--- screen closes (it is also dropped when the mod unloads).
---@param on boolean
function GearUi.nav_modal(on) end

--- Replaces the game's window `name` with the mod's own screen: while the window is open, the game neither draws it
--- nor gives it the mouse or the keys; it still opens and closes as it always does (hotkeys, the escape menu), so the
--- game's state stays the one it expects. Names: the in-game windows by control name (`UI_WND_SAVEGAME`, ...) and the
--- menus' screens `UI_MENU_MAIN`, `UI_MENU_CHARACTER`, `UI_MENU_OPTIONS`, `UI_MENU_LOADGAME`, `UI_MENU_NETWORK`.
--- One mod per window. The game's window comes back by itself when the mod does not draw its screen: call `replaced`
--- every tick it does; 1.5 s after the window opened without it, or 0.5 s after the last call, the game's window shows
--- until it closes. A mod that is off or unloaded replaces nothing. `on = false` gives the window back.
---@param name string
---@param on? boolean
---@return boolean ok
---@return string? error
function GearUi.replace_window(name, on) end

--- Call it every tick the mod draws its screen for a replaced window: true while the game's window is open and the
--- mod's screen takes its place (draw it then); false when the window is closed or the game's own shows.
---@param name string
---@return boolean
function GearUi.replaced(name) end

--- Whether the game's window (an in-game control name or a menu screen, as for `replace_window`) is open.
---@param name string
---@return boolean
function GearUi.window_open(name) end

--- Closes the game's window as its own Back button or Esc does (the savegame window in the menus brings the start
--- menu back). False if it is not open.
---@param name string
---@return boolean
function GearUi.close_window(name) end

--- The controller's B was pressed: true once per press (consumed). B closes an open dropdown and the topmost closable
--- panel by itself; a `nav_modal` screen or a panel with `back = false` asks here.
---@return boolean
function GearUi.back_pressed() end

--- An entry of the controller's mod menu (Sacred Gear's mod menu button, LT + LB by default): for a tool a player
--- would otherwise reach with a hotkey, Alt or a small button. The keys a mod declares in mod.lua are listed there
--- by themselves; calling it again with the same id changes the entry.
---@param id string
---@param label string
---@param fn fun()
function GearUi.menu_item(id, label, fn) end

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
