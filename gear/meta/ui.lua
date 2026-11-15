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
---@field color? GearColor Tint: white as is; the alpha makes it translucent.

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
---@param id integer An id from `images`.
---@param x number
---@param y number
---@param w? number
---@param h? number
---@param opts? GearImageOpts
---@return boolean drawn
function GearUi.image(id, x, y, w, h, opts) end

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

--- The cursor in layout coordinates.
---@return number x
---@return number y
function GearUi.mouse() end

---@alias GearKey 'escape'|'enter'|'space'|'tab'|'shift'|'control'|'alt'|'left'|'up'|'right'|'down'|'insert'|'delete'|'home'|'end'|'pageup'|'pagedown'|'f1'|'f2'|'f3'|'f4'|'f5'|'f6'|'f7'|'f8'|'f9'|'f10'|'f11'|'f12'|'a'|'b'|'c'|'d'|'e'|'f'|'g'|'h'|'i'|'j'|'k'|'l'|'m'|'n'|'o'|'p'|'q'|'r'|'s'|'t'|'u'|'v'|'w'|'x'|'y'|'z'|'0'|'1'|'2'|'3'|'4'|'5'|'6'|'7'|'8'|'9'|integer

--- True once per press of the key, read while the game's window has the focus. Also sees the keys automation holds.
---@param name GearKey A name or a virtual key code.
---@return boolean
function GearUi.key_pressed(name) end

--- Whether the key is held.
---@param name GearKey
---@return boolean
function GearUi.key_down(name) end
