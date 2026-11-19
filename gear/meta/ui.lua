---@meta _
-- Sacred Gear: drawing on top of the game (gear.ui), its state queries and the layout.

-- ## Drawing (gear.ui)

--- A color: an `0xAARRGGBB` number (see `gear.ui.rgba`), `{r, g, b, a}` or `{r = , g = , b = , a = }` (0..255, `a`
--- defaults to 255).
---@alias GearColor integer|integer[]|{r: integer, g: integer, b: integer, a: integer?}

--- Where in the game's draw order. `world`: in game, over the world and under every window. `hud` (default): over the
--- game's windows, under tooltips, the escape menu and message boxes. `top`: over everything but the cursor and its
--- tooltip.
---@alias GearLayer 'world'|'hud'|'top'

---@alias GearTextAlign 'left'|'center'|'right'
---@alias GearTextVAlign 'top'|'middle'|'bottom'|'baseline'
---@alias GearImageFit 'stretch'|'contain'|'cover'|'fit'|'fill'

--- Options of every draw function.
---@class GearDrawOpts
---@field layer? GearLayer Default `'hud'`.
---@field window? string A control name (`UI_WND_INVENTORY`): the command is drawn in that window's coordinates and frame, so it moves with the window, and clipped to its 1024x768 frame; nothing is drawn while the window does not exist.

--- Options of `gear.ui.text` (and the font options of widgets and `text_size`).
---@class GearTextOpts : GearDrawOpts
---@field color? GearColor Default white.
---@field font? 'ui'|'game1'|'game2'|'game3'|'game4'|'game5'|'game6'|'game7'|integer Default: the vector UI font (Segoe UI). `'game1'`..`'game7'` (or the number 1..7; 0 is a debug font) draw in the game's own bitmap font, scaled with the UI.
---@field size? number The em height in layout units (vector font; default 13).
---@field face? string A Windows font name (vector font).
---@field bold? boolean Vector font.
---@field italic? boolean Vector font.
---@field shadow? boolean Vector font: a dark 1 pixel shadow.
---@field outline? boolean Vector font: a dark 1 pixel outline.
---@field width? number The box width, for `align`.
---@field height? number The box height, for `valign`.
---@field align? GearTextAlign Aligns in `width`.
---@field valign? GearTextVAlign Aligns in `height`; `'middle'` puts the capitals' middle on the box's, `'baseline'`: y is the first line's baseline.
---@field background? GearColor A rectangle behind the text, sized to it.
---@field padding? number Around the text, with `background`.
---@field markup? boolean Game fonts: pass their backslash codes (`\c` color, `\g` image) through; otherwise a `\` shows as `/`.
---@field flags? integer Raw font flags of the game's font (or-ed into `align`).

---@class GearImageOpts : GearDrawOpts
---@field fit? GearImageFit `'stretch'` (default: fills the box), `'contain'` (the whole image, aspect kept, centered), `'cover'` (fills the box, aspect kept, the overhang cut off).
---@field flip? 'h'|'v'|'hv' Mirrored left-right (`'h'`), upside down (`'v'`) or both.
---@field color? GearColor Tint: white as is; the alpha makes it translucent.
---@field crop? number[] `{left, top, right, bottom}` as fractions of the image (default `{0, 0, 1, 1}`): only that part is drawn, where it would be in the box. `{0, 0.6, 1, 1}` draws the lower 40 % (a gauge).

---@class GearImageInfo
---@field id integer The game's image index (stable for a build).
---@field sheet string The sheet's TGA name.
---@field width integer
---@field height integer
---@field loaded boolean

-- path gear.ui
--- Drawing is immediate mode: what a mod draws during a tick (its `draw` and `frame` handlers, timers, tasks) is shown
--- until the next tick replaces it, so a mod draws its overlay every frame, usually in a `draw` handler. Without a tick
--- (a loading screen, the menus without automation) nothing is drawn after half a second.
---
--- Positions and sizes are the UI's 1024x768 layout, the space the game lays its windows out in; the game and SacredBild
--- scale it to the screen. A mod may issue 2000 draw commands per tick (widgets count their rectangles and texts).
---@class GearUi
---@field WIDTH integer 1024
---@field HEIGHT integer 768
---@field COLORS GearColors The widgets' colours.
---@field native GearUiNative Pieces of the game's own look built from its UI images (`gear.ui.native.frame`).
local GearUi = {}

---@class GearColors
---@field bg integer
---@field bg_hover integer
---@field bg_down integer
---@field edge integer
---@field edge_dim integer
---@field text integer
---@field text_dim integer
---@field accent integer
---@field title integer

--- The color number `0xAARRGGBB` (`a` defaults to 255).
---@param r integer
---@param g integer
---@param b integer
---@param a? integer
---@return integer
function GearUi.rgba(r, g, b, a) end

--- A filled rectangle (default color: black, 75 % opaque). Edges snap to the screen's pixels.
---@param x number
---@param y number
---@param w number
---@param h number
---@param color? GearColor
---@param opts? GearDrawOpts
---@return boolean drawn False when nothing was drawn (its `window` does not exist).
function GearUi.rect(x, y, w, h, color, opts) end

--- An outline inside the box.
---@param x number
---@param y number
---@param w number
---@param h number
---@param color? GearColor
---@param thickness? number Default 1.
---@param opts? GearDrawOpts
---@return boolean drawn
function GearUi.frame(x, y, w, h, color, thickness, opts) end

--- Text, UTF-8, lines break at `\n`. By default in a vector UI font (Segoe UI) rasterized for the screen's pixels, so it
--- stays sharp however far the UI is scaled.
---@param text any Converted with `tostring`.
---@param x number
---@param y number
---@param opts? GearTextOpts
---@return boolean drawn
function GearUi.text(text, x, y, opts) end

--- A UI image; `w`, `h` default to its own size. Images are filtered linearly.
---@param id integer An id from `images`, or a picture id (`gear.game.save_picture`).
---@param x number
---@param y number
---@param w? number
---@param h? number
---@param opts? GearImageOpts
---@return boolean drawn
function GearUi.image(id, x, y, w, h, opts) end

--- What the game's inventory windows show for an item, its own 3D model, drawn by the game's model renderer as large as
--- fits the box (x, y, w, h) and centered in it (the inventory's own size is 32 units a cell). Give an item (a table from
--- `gear.world.item` or a container's `items()`: the live object is drawn, with its own state), or a type id or name:
--- then a temporary item of that type is made at the hero's feet for the frame and destroyed after the layer's drawing
--- (a type id or name needs no item object: its model pair comes from the model loader; no tint).
--- Same `layer` and `window` options as the other draws.
---@param item GearItem|integer|string An item, a type id or a type name.
---@param x number
---@param y number
---@param w number
---@param h number
---@param opts? GearDrawOpts
---@return boolean drawn False when it could not be queued (a bad window or layer); whether the model really draws is known to the game's renderer only.
function GearUi.item_icon(item, x, y, w, h, opts) end

--- The game's own tooltip popup for an item, as the inventory windows show it (framed, the name in the rarity colour,
--- price, protection, level, bonus lines), centered on (x, y) of the 1024x768 layout and kept on the screen. `item` is
--- an item (a table from `gear.world.item` or a container's `items()`), an item id, or the string `gear.inv.export`
--- made (a temporary item is made to read the text from and destroyed at once, local-only in a network game). Call it every
--- frame the tooltip should be shown; it is drawn by the game's popup pass. `x`, `y` are the popup's top-left corner.
--- It also returns the popup's size as of its last layout (what the previous tooltip it showed took; 0 at first), so
--- a mod can place the next one beside a region.
---@param item GearItem|integer|string
---@param x number
---@param y number
---@return boolean? ok
---@return number|string? width_or_error The popup's width in layout units, or the message when `ok` is nil.
---@return number? height
function GearUi.item_tooltip(item, x, y) end

--- The words of the game's item tooltip (UTF-8, with the game's colour codes such as `\c...`) instead of a popup, for the
--- same arguments as `item_tooltip`. Reading an export needs no registered object: a scratch item the object factory makes
--- is filled with the data, asked and deleted, with no object id, no object manager and nothing sent to the network.
---@param item GearItem|integer|string
---@return string? text
---@return string? error
function GearUi.item_tooltip_text(item) end

--- The item whose tooltip the game shows right now: the cursor (the mouse's or the controller's pad cursor) rests on an item
--- in the backpack, the chest, a shop, the equipment window ... Read as of the last frame; nil when no item tooltip is up.
--- Also returns the tooltip's visible outer rectangle (top-left corner and size of the frame's bars; the corner ornaments reach
--- a few units beyond it) in the same layout units `gear.ui.*` draws in, so a mod can put something beside it (see
--- `tooltip_anchor`). The game's own popup numbers are in another space (the canvas, whose origin is the screen's left edge
--- instead of the layout's) and a little smaller than the frame; Sacred Gear converts them.
---@return GearItem? item
---@return number? x
---@return number? y
---@return number? w
---@return number? h
function GearUi.hovered_item() end

---@class GearTooltipAnchorOpts
---@field gap? number More space between the two frames than the touching ornaments (default 0).
---@field margin? number Distance kept from the screen's edges (default 2).
---@field side? 'right'|'left' Force a side; by default the right one, the left one when the panel does not fit on the right.

--- Where a panel goes to sit flush against the game's item tooltip: the top-left corner `x`, `y` of a panel whose frame's bars
--- are `w` x `h` (what `gear.ui.native.frame` takes), beside the tooltip `hovered_item` reports, with the corner ornaments of
--- both frames touching. On the right of the tooltip, on its left when it does not fit, never outside the screen, its top
--- level with the tooltip's (moved up or down to fit). nil when no item tooltip is up. Also returns the side used.
---@param w number
---@param h number
---@param opts? GearTooltipAnchorOpts
---@return number? x
---@return number? y
---@return ('right'|'left')? side
function GearUi.tooltip_anchor(w, h, opts) end

--- A texture of the game's Texture.pak by its file name (`GUI_MOVE_RUNDUM.TGA`, any case) as an image id for `image`; nil if the
--- game has none. The engine loads it and it keeps its own size. The icons of combat arts and spells are such textures
--- (`gear.classes.art_icon`).
---@param name string
---@return integer? id
function GearUi.texture_image(name) end

--- The UI images whose sheet name contains `filter` (any case), all of them without a filter.
---@param filter? string
---@return GearImageInfo[]
function GearUi.images(filter) end

--- One UI image, or nil.
---@param id integer
---@return GearImageInfo?
function GearUi.image_info(id) end

--- The number of the game's UI fonts.
---@return integer
function GearUi.fonts() end

--- Width and height of the text in the layout, or nil. Game fonts in game only (measuring may add glyphs to the font).
---@param text any
---@param font? 'ui'|'game1'|'game2'|'game3'|'game4'|'game5'|'game6'|'game7'|integer|GearTextOpts A game font, or a table of `text`'s font options; nil: the default UI font.
---@return number? width
---@return number? height
function GearUi.text_size(text, font) end

-- ## State queries

--- The state of the UI, also the argument of `draw` handlers.
---@class GearUiState
---@field available boolean
---@field mode 'game'|'menus'|'cinematic'|'none'
---@field exclusive? string The control name of the window the game draws alone (options, save, character, world map), if any.
---@field escape_menu boolean
---@field message_box boolean
---@field help boolean
---@field show_text boolean The game shows its item and object labels ("show text": Alt held), in game; as of the last frame drawn.
---@field mouse_x number The cursor in layout coordinates.
---@field mouse_y number
---@field screen_left number The screen's edges in layout coordinates (beyond 0..1024 x 0..768 when SacredBild's canvas leaves room around it).
---@field screen_top number
---@field screen_right number
---@field screen_bottom number

--- One of the game's in-game windows, in the window's own layout coordinates.
---@class GearWindow
---@field name string The control name (`UI_WND_INVENTORY`).
---@field slot integer
---@field x number
---@field y number
---@field width number
---@field height number
---@field visible boolean
---@field drawn boolean Open and drawn this frame (not hidden behind a window the game draws alone).
---@field frame_x number Where the window's frame puts its layout: its place on the canvas is `x + frame_x, y + frame_y`.
---@field frame_y number
---@field movable boolean
---@field bounds? GearRect With SacredBild: where the window drew in the last frame on the canvas, its tabs and ornaments included.

---@class GearRect
---@field x number
---@field y number
---@field w number
---@field h number

--- The game's in-game window by control name (any case), or nil.
---@param name string
---@return GearWindow?
function GearUi.window(name) end

--- The game's in-game windows.
---@return GearWindow[]
function GearUi.windows() end

---@return GearUiState
function GearUi.state() end

--- The screen's edges in layout coordinates.
---@return number left
---@return number top
---@return number right
---@return number bottom
function GearUi.screen() end

--- The cursor in layout coordinates. In a tick with a button press or release over a region: where that happened.
---@return number x
---@return number y
function GearUi.mouse() end

--- Whether the cursor is over a region of a mod's UI (a panel, a button), as of the last frame.
---@return boolean
function GearUi.over_ui() end

---@alias GearKey 'escape'|'enter'|'space'|'tab'|'shift'|'control'|'alt'|'left'|'up'|'right'|'down'|'insert'|'delete'|'home'|'end'|'pageup'|'pagedown'|'f1'|'f2'|'f3'|'f4'|'f5'|'f6'|'f7'|'f8'|'f9'|'f10'|'f11'|'f12'|'a'|'b'|'c'|'d'|'e'|'f'|'g'|'h'|'i'|'j'|'k'|'l'|'m'|'n'|'o'|'p'|'q'|'r'|'s'|'t'|'u'|'v'|'w'|'x'|'y'|'z'|'0'|'1'|'2'|'3'|'4'|'5'|'6'|'7'|'8'|'9'|integer

--- True once per press of the key, read while the game's window has the focus. Also sees the keys automation holds.
---@param name GearKey A name or a virtual key code.
---@return boolean
function GearUi.key_pressed(name) end

--- While `on`, the keyboard does not reach the game: its key and character messages are dropped (with the release of
--- every key whose press was dropped) and its key state queries see nothing held, so hotkeys and typing stay out of the
--- game. The mod's own `key_pressed` and `key_down` still work, the mouse is not affected. For a screen that types or
--- binds keys; it ends when the mod unloads. Not tied to `gear.keys`: declared keys of other mods stay quiet only for the
--- Mods screen.
---@param on boolean
function GearUi.capture_keys(on) end

--- What the player typed since the last tick while this mod captures the keys (`capture_keys`): the characters their
--- keyboard layout makes (umlauts, Shift, AltGr), UTF-8, no control characters (read Backspace and Enter with
--- `key_pressed`). Empty otherwise. For a text field.
---@return string
function GearUi.typed() end

--- Whether the key is held.
---@param name GearKey
---@return boolean
function GearUi.key_down(name) end

-- ## The game's own look (gear.ui.native)

-- path gear.ui.native
--- Pieces of the game's own look, drawn with its UI images (`gear.ui.images`) through `gear.ui.image` and `gear.ui.rect`: layers,
--- windows, the draw command budget and the 1024x768 layout work as for those. Nothing here needs a game font; the text is the
--- mod's. A style is a table of image ids and sizes inside Sacred Gear, so a further piece (a ring, a bar) is one more style.
---@class GearUiNative
local GearUiNative = {}

---@class GearNativeFrameOpts : GearDrawOpts
---@field style? 'tooltip' The frame style: `'tooltip'` (default) is the border of the game's item tooltip, dark metal bars with ornaments in the four corners (image 54 top-left, 55 top-right, flipped upside down at the bottom; 52 for the bars).
---@field fill? GearColor|false The fill inside the bars (default the game's translucent black `0x9F000000`); `false` draws none.
---@field color? GearColor Tint of the border images (white as is).

--- A frame in the game's own style: the fill, the four bars and the four corner ornaments. `x`, `y`, `w`, `h` are the bars'
--- outer rectangle (the fill's too); the corner ornaments reach a few units beyond it (see `insets`), like the game's
--- tooltip, whose own frame is this one. Put the contents inside `x + inset.left` ... and leave `overhang` of room if the
--- ornaments must not cover anything.
---@param x number
---@param y number
---@param w number
---@param h number
---@param opts? GearNativeFrameOpts
---@return boolean drawn
function GearUiNative.frame(x, y, w, h, opts) end

--- How far the border of a frame style reaches into its box (the text goes inside that) and how far its corner ornaments
--- reach out of the box, both as `{left, top, right, bottom}` in layout units.
---@param style? 'tooltip' Default `'tooltip'`.
---@return {left: number, top: number, right: number, bottom: number} inset
---@return {left: number, top: number, right: number, bottom: number} overhang
function GearUiNative.insets(style) end
