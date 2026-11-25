-- gear.kit: the core widgets of RmlUi screens, as markup. Shipped inside Sacred Gear and run once per mod, in the mod's own environment.
--
-- A widget here is a string of RML that only uses the class names every look understands: the plain base style of Sacred Gear (no theme mod
-- running) and the theme mods (sacred-theme) both style `.window`, `.titlebar`, `.tabbar`, `.row`, `.game-button`, `.game-check`, `.game-slider`,
-- `.bar`, `.list`, `.padkey`. So a screen built from the kit works and reads well without a theme, and looks like the game with one. Every
-- button, link and input is a stop for the controller (no extra work), and the markup carries no look of its own.
--
-- Compose: `kit.window{title = 'Hello', id = 'win', x = 100, y = 80, width = 360, body = kit.row('Volume', kit.slider('vol', 0, 10, 1, 5)) .. kit.button('ok', 'OK', {primary = true})}`
-- gives the markup of one window; `kit.page(markup, {head = '<link type="text/rcss" href="my.rcss"/>'})` a whole document for `gear.ui.rml.load_string`.
-- Text arguments are escaped; the arguments named `body`, `content` and `control` are markup and are not.

local ui = gear.ui
local K = {}

local ESC = {['&'] = '&amp;', ['<'] = '&lt;', ['>'] = '&gt;', ['"'] = '&quot;'}

-- kit.esc(text) -> text safe in RML (also in an attribute)
function K.esc(text)
    return (tostring(text == nil and '' or text):gsub('[&<>"]', ESC))
end
local esc = K.esc

local function attrs(t)
    local out = {}
    for _, kv in ipairs(t) do
        local v = kv[2]
        if v ~= nil and v ~= false then out[#out + 1] = ' ' .. kv[1] .. '="' .. esc(v == true and kv[1] or v) .. '"' end
    end
    return table.concat(out)
end

local function classes(base, extra)
    return extra and extra ~= '' and (base .. ' ' .. extra) or base
end

local function num(v)
    local s = ('%.6f'):format(v):gsub('0+$', ''):gsub('%.$', '')
    return s
end

-- kit.window{title, id, x, y, width, height, panel, close, class, body} -> markup
-- A frame in the look of the game's tooltip window with a title bar and a body. With `x` / `y` it is placed (layout units); `panel = 'name'` makes it a
-- window the layout editor can move (data-panel; `resizable = true` for the corner grip); `close = 'id'` adds a close button (id) to the title bar.
-- A window with a title and an `id` is a panel named after its id (`panel = false` keeps it fixed): the player drags it by its title bar too, and
-- the place is kept like the layout editor's.
function K.window(o)
    local panel = o.panel
    if panel == nil and o.title and o.id then panel = o.id end
    if not panel then panel = nil end
    local style = {}
    if o.x or o.y then style[#style + 1] = 'position: absolute; left: ' .. num(o.x or 0) .. 'px; top: ' .. num(o.y or 0) .. 'px;' end
    if o.width then style[#style + 1] = 'width: ' .. num(o.width) .. 'px;' end
    if o.height then style[#style + 1] = 'height: ' .. num(o.height) .. 'px;' end
    local close = o.close and ('<button id="' .. esc(o.close) .. '" class="ibtn close act" data-no-padkey="" style="float: right; margin: 0;"><span class="icon icon-close"></span></button>') or ''
    return '<div' .. attrs({{'id', o.id}, {'class', classes('window', o.class)}, {'data-panel', panel}, {'data-title', panel and (o.title or panel)},
        {'data-resizable', o.resizable}, {'style', #style > 0 and table.concat(style, ' ')}}) .. '>'
        .. (o.title and ('<div class="titlebar">' .. close .. esc(o.title) .. '</div>') or '')
        .. '<div class="body">' .. (o.body or '') .. '</div></div>'
end

-- kit.tabs{id, tabs = {{id = 'a', label = 'One'}, ...}, active = 'a'} -> markup: the tab header bar (`<a class="link tab">`, id = '<id>-<tab id>').
function K.tabs(o)
    local parts = {}
    for i, t in ipairs(o.tabs or {}) do
        if i > 1 then parts[#parts + 1] = '<span class="tab-sep"></span>' end
        parts[#parts + 1] = '<a' .. attrs({{'id', (o.id or 'tab') .. '-' .. t.id}, {'class', classes('link tab', t.id == o.active and 'on' or nil)}}) .. '>' .. esc(t.label) .. '</a>'
    end
    return '<div class="tabbar">' .. table.concat(parts) .. '</div>'
end

-- kit.button(id, label, {primary, danger, class, disabled}) -> markup (a game button; every button gets its controller prompt from Gear).
function K.button(id, label, o)
    o = o or {}
    local cls = classes('game-button', (o.primary and 'primary ' or '') .. (o.danger and 'danger ' or '') .. (o.class or ''))
    return '<button' .. attrs({{'id', id}, {'class', (cls:gsub('%s+$', ''))}, {'disabled', o.disabled}}) .. '>' .. esc(label) .. '</button>'
end

-- kit.dropdown(id, label, {tip, disabled}) -> markup: the button of a drop-down (the game's arrow caps with the label between; no chevron in the label). The
-- list it opens is the mod's: build it with kit.list and show it under the button while the mod's own state says so.
function K.dropdown(id, label, o)
    o = o or {}
    return '<button' .. attrs({{'id', id}, {'class', 'game-dropdown'}, {'title', o.tip}, {'disabled', o.disabled}}) .. '>' .. esc(label) .. '</button>'
end

-- kit.icon(name, {class}) -> markup: an icon of Sacred Gear's icon font (Material Symbols; names like 'refresh', 'arrow-back', 'delete', 'chevron-left', see docs/RmlKit.md).
function K.icon(name, o)
    return '<span class="' .. classes('icon icon-' .. tostring(name):gsub('_', '-'), o and o.class) .. '"></span>'
end

-- kit.check(id, label, checked, {disabled, tip}) -> markup: a row with the label and the game's check orb (an `input`, so `doc:on('#id', 'change', fn)`).
function K.check(id, label, checked, o)
    o = o or {}
    return '<div class="check-row"' .. attrs({{'title', o.tip}}) .. '><span class="text">' .. esc(label) .. '</span><input type="checkbox" class="game-check"'
        .. attrs({{'id', id}, {'checked', checked and true or nil}, {'disabled', o.disabled}}) .. '/></div>'
end

-- kit.slider(id, min, max, step, value, {class}) -> markup: the game's slider (its `change` event has `params.value`).
function K.slider(id, min, max, step, value, o)
    o = o or {}
    return '<input type="range" class="' .. classes('game-slider', o.class) .. '"' .. attrs({{'id', id}, {'min', num(min)}, {'max', num(max)}, {'step', num(step)}, {'value', num(value)}}) .. '/>'
end

-- kit.row(label, control, {tip, class}) -> markup: a grey label at the left and the control (markup) at the right, the way settings screens list things.
function K.row(label, control, o)
    o = o or {}
    return '<div' .. attrs({{'class', classes('row space', o.class)}, {'title', o.tip}}) .. '><span class="label">' .. esc(label) .. '</span><span>' .. (control or '') .. '</span></div>'
end

-- kit.heading(text) -> markup: a section title.
function K.heading(text) return '<div class="heading">' .. esc(text) .. '</div>' end

-- kit.text(text, {class}) -> markup: a paragraph (`class = 'muted'`, 'accent', 'good' or 'bad' colour it).
function K.text(text, o) return '<div' .. attrs({{'class', o and o.class}}) .. '>' .. esc(text) .. '</div>' end

-- kit.progress(fraction, {class, id, width}) -> markup: a bar `width` wide (default 200 layout units) filled to `fraction` (0 to 1); class 'mana' or 'xp' for the other colours.
function K.progress(fraction, o)
    o = o or {}
    fraction = math.max(0, math.min(1, tonumber(fraction) or 0))
    return '<div' .. attrs({{'id', o.id}, {'class', classes('bar', o.class)}, {'style', 'width: ' .. num(o.width or 200) .. 'px;'}}) .. '><div class="fill" style="width: ' .. num(fraction * 100) .. '%"></div></div>'
end

-- kit.list(items, {class}) -> markup: items = {{id = 'a', name = 'Title', sub = 'smaller grey line', selected = false}, ...}; each item is a button (a pad stop).
function K.list(items, o)
    local parts = {}
    for _, it in ipairs(items or {}) do
        parts[#parts + 1] = '<button' .. attrs({{'id', it.id}, {'class', classes('item', it.selected and 'selected' or nil)}, {'data-no-padkey', true}}) .. '><div class="name">'
            .. esc(it.name) .. '</div>' .. (it.sub and ('<div class="sub">' .. esc(it.sub) .. '</div>') or '') .. '</button>'
    end
    return '<div class="' .. classes('list', o and o.class) .. '">' .. table.concat(parts) .. '</div>'
end

-- kit.buttons(...) -> markup: buttons side by side, at the right (pass the markup of kit.button).
function K.buttons(...) return '<div class="row right">' .. table.concat({...}) .. '</div>' end

-- kit.page(markup, {head}) -> the markup of a whole document around `markup` (windows, absolute positioned), for gear.ui.rml.load_string.
function K.page(markup, o)
    -- (A <body> is as wide as its content unless told otherwise, and an absolutely positioned window with no width then shrinks to nothing: the body fills the screen.)
    return '<rml><head><style>body { width: 100%; }</style>' .. ((o and o.head) or '') .. '</head><body>' .. markup .. '</body></rml>'
end

-- kit.ibtn(id, icon, {tip, label, class, disabled}) -> markup: a frameless icon button (`ibtn`) with the pad prompt in front, optionally with a text after the icon.
-- `kit.ibtn('kbd', 'keyboard', {tip = 'On-screen keyboard'})` is the button that opens a kit.osk.
function K.ibtn(id, icon, o)
    o = o or {}
    return '<button' .. attrs({{'id', id}, {'class', classes('ibtn', o.class)}, {'title', o.tip}, {'disabled', o.disabled}}) .. '><span class="padkey a"></span>'
        .. K.icon(icon) .. (o.label and (' ' .. esc(o.label)) or '') .. '</button>'
end

-- On-screen keyboard -----------------------------------------------------------------------------------------------------------------------------
-- kit.osk{title, layout, x, y, width, modal, back} -> keyboard. One keyboard per mod is enough: it is a small document of its own (a bordered window with
-- the typed text, rows of bordered key buttons and Shift / Space / Backspace / Clear / Paste / Done) that floats at the bottom of the screen while it is open.
-- Every key is a stop for the controller (D-pad, A types, B closes) and the mouse works too.
--   kb:open{text, digits, max, title, get, on_change, on_close}   open it for a text; `digits = true` is a number pad (digits only, also from the clipboard);
--                                                                 `max` is the longest text in characters (default 64); `on_change(text)` runs after every edit,
--                                                                 `on_close(text, how)` once when it closes (how = 'done', 'back', 'close' or 'closed'); `get()`
--                                                                 returns what the mod's own field holds now (when the player types there, the keyboard follows).
--   kb:toggle(opts)  open it, or close it when it is open;   kb:close()   kb:is_open()   kb:text()   kb:set_text(text)
--   kb:back() -> boolean   closes it when open and returns true: call it first in the mod's own handling of B / Escape so the one press closes the keyboard only.
--   kb:draw()  call it every frame from the mod's draw handler (it does nothing while closed).
-- The keyboard asks `ui.back_pressed()` and Escape itself unless it is made with `back = false` (a mod that handles B itself calls `kb:back()` instead).
local OSK_LAYOUTS = {
    qwerty = {
        {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
        {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
        {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'},
        {'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', "'"},
    },
    qwertz = {
        {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'ß'},
        {'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 'ü'},
        {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'ö', 'ä'},
        {'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', "'"},
    },
}
local OSK_PAD = {{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}}
local OSK_SHIFTED = {['ü'] = 'Ü', ['ö'] = 'Ö', ['ä'] = 'Ä', [','] = ';', ['.'] = ':', ['-'] = '_', ["'"] = '"', ['ß'] = '?'}

local function char_size(b) return b >= 240 and 4 or b >= 224 and 3 or b >= 192 and 2 or 1 end
local function ulen(s) return select(2, s:gsub('[^\128-\191]', '')) end
-- the first n characters of a UTF-8 text
local function usub(s, n)
    local i, count = 1, 0
    while i <= #s do
        count = count + 1
        if count > n then return s:sub(1, i - 1) end
        i = i + char_size(s:byte(i))
    end
    return s
end
-- the last n characters of a UTF-8 text
local function utail(s, n)
    local skip = ulen(s) - n
    local i = 1
    while skip > 0 and i <= #s do
        i = i + char_size(s:byte(i))
        skip = skip - 1
    end
    return s:sub(i)
end
local function upop(s) return (s:gsub('[%z\1-\127\194-\244][\128-\191]*$', '')) end

-- kit.osk_clean(text, digits) -> text: what the clipboard may put into a field: no line breaks or tabs (each run is one space), no other control characters,
-- no blanks at either end; a number pad takes digits only.
local function osk_clean(text, digits)
    text = tostring(text or ''):gsub('[\r\n\t]+', ' '):gsub('[%z\1-\31\127]', '')
    if digits then return (text:gsub('%D', '')) end
    return (text:gsub('^%s+', ''):gsub('%s+$', ''))
end
K.osk_clean = osk_clean

function K.osk(cfg)
    cfg = cfg or {}
    local kb = {}
    local doc, st
    local actions = {}          -- element id -> function
    local keys = {}             -- {id, ch} of the letter keys, for the shift labels

    local function key_label(ch, shift)
        if not shift then return ch end
        return OSK_SHIFTED[ch] or ch:upper()
    end

    local function refresh()
        if not doc then return end
        local t = utail(st.text, 30)
        doc:text('#kit-osk-text', (t ~= st.text and '…' or '') .. t)
        doc:text('#kit-osk-count', st.max_given and (ulen(st.text) .. '/' .. st.max) or '')
    end

    local function drop(how)
        local text, cb = st.text, st.on_close
        st = nil
        if doc then doc:close(); doc = nil end
        if cb then cb(text, how) end
    end

    local function set_text(text)
        st.text = text
        refresh()
        if st.on_change then st.on_change(text) end
    end

    local function type_text(s)
        local room = st.max - ulen(st.text)
        if room <= 0 or s == '' then return end
        set_text(st.text .. usub(s, room))
    end

    local function paste()
        local clip = gear.clipboard and gear.clipboard.get()
        if clip then type_text(osk_clean(clip, st.digits)) end
    end

    local function set_shift(on)
        st.shift = on
        for _, k in ipairs(keys) do doc:text('#' .. k.id, key_label(k.ch, on)) end
        doc:class('#kit-osk-shift', 'on', on)
    end

    local n = 0
    local function key(ch, class)
        n = n + 1
        local id = 'kit-osk-k' .. n
        keys[#keys + 1] = {id = id, ch = ch}
        actions[id] = function()
            type_text(key_label(ch, st.shift))
            if st.shift then set_shift(false) end
        end
        return '<button id="' .. id .. '" class="' .. classes('osk-key', class) .. '" data-no-padkey="">' .. esc(key_label(ch, st.shift)) .. '</button>'
    end
    local function action(id, class, label, fn, padkey)
        actions['kit-osk-' .. id] = fn
        return '<button id="kit-osk-' .. id .. '" class="' .. classes('osk-key', class) .. '"' .. (padkey and '' or ' data-no-padkey=""') .. '>' .. label .. '</button>'
    end
    local function row(cells) return '<div class="osk-row">' .. table.concat(cells) .. '</div>' end

    local function body()
        actions, keys, n = {}, {}, 0
        local rows = {}
        local function backspace() set_text(upop(st.text)) end
        local function clear() set_text('') end
        local function done() drop('done') end
        local back_icon = K.icon('backspace')
        if st.digits then
            for _, r in ipairs(OSK_PAD) do
                local cells = {}
                for _, ch in ipairs(r) do cells[#cells + 1] = key(ch, 'pad') end
                rows[#rows + 1] = row(cells)
            end
            rows[#rows + 1] = row{action('clear', 'pad txt', 'Clear', clear), key('0', 'pad'), action('back', 'pad', back_icon, backspace)}
            rows[#rows + 1] = row{action('paste', 'half', K.icon('content-paste') .. ' Paste', paste), action('done', 'half done', 'Done', done, true)}
        else
            for _, r in ipairs(OSK_LAYOUTS[cfg.layout or 'qwerty'] or OSK_LAYOUTS.qwerty) do
                local cells = {}
                for _, ch in ipairs(r) do cells[#cells + 1] = key(ch) end
                rows[#rows + 1] = row(cells)
            end
            rows[#rows + 1] = row{
                action('shift', 'wide', 'Shift', function() set_shift(not st.shift) end),
                action('space', 'space', 'Space', function() type_text(' ') end),
                action('back', 'wide', back_icon, backspace),
                action('clear', 'wide', 'Clear', clear),
                action('paste', 'wide', K.icon('content-paste'), paste),
                action('done', 'wide done', 'Done', done, true),
            }
        end
        return '<div class="osk-line"><span id="kit-osk-text" class="osk-text"></span><span class="osk-caret">_</span><span id="kit-osk-count" class="osk-count"></span></div>'
            .. table.concat(rows)
    end

    local function build()
        local content = body()
        local markup = K.page(K.window{id = 'kit-osk', title = st.title or cfg.title or 'Keyboard', width = cfg.width,
            class = 'kit-osk', panel = false, x = 0, y = 0, close = 'kit-osk-close', body = content})
        doc = ui.rml.load_string(markup, {})
        if not doc then drop('closed') return end
        actions['kit-osk-close'] = function() drop('close') end
        doc:on('button', 'click', function(e)
            local fn = st and actions[tostring(e.current)]
            if fn then fn() end
        end)
        refresh()
    end

    function kb:is_open() return st ~= nil end
    function kb:text() return st and st.text or nil end
    function kb:set_text(text)
        if not st then return end
        st.text = usub(tostring(text or ''), st.max)
        refresh()
    end
    function kb:close() if st then drop('close') end end
    function kb:back()
        if not st then return false end
        drop('back')
        return true
    end

    function kb:open(o)
        o = o or {}
        if st then drop('closed') end
        st = {digits = o.digits and true or false, max = o.max or 64, max_given = o.max ~= nil, title = o.title, get = o.get,
            on_change = o.on_change, on_close = o.on_close, shift = false}
        st.text = usub(tostring(o.text or ''), st.max)
        build()
    end
    function kb:toggle(o)
        if st then drop('close') else kb:open(o) end
    end

    -- kb:attach(doc, {field = '#id', button = '#kbd-id', open = fn}): the keyboard of a text field, for the pad. The keyboard button (class `kit-kbd`, which both
    -- looks show only while the controller is the player's input) opens it, and so does a click (A) on the field itself while that button is shown; with the
    -- mouse and keys the field stays a plain field. `open()` calls `kb:toggle{...}` for the field (see kit.field for one made from the element's own value).
    function kb:attach(d, o)
        local function shown()
            local b = d:query(o.button)
            local r = b and b:rect()
            return r ~= nil and r.w > 0 and r.h > 0
        end
        d:on(o.button, 'click', function() o.open() end)
        d:on(o.field, 'click', function() if not st and shown() then o.open() end end)
    end

    function kb:draw()
        if not st then return end
        if cfg.back ~= false and (ui.back_pressed() or ui.key_pressed('escape')) then drop('back') return end
        if st.get then
            local t = st.get()
            if type(t) == 'string' and t ~= st.text then st.text = usub(t, st.max); refresh() end
        end
        if not doc then return end
        -- at the bottom of the screen in the middle (or where `x` / `y` say), once its size is known
        local box = doc:query('#kit-osk')
        local r = box and box:rect()
        if r then
            local l, t, rr, b = ui.screen()
            doc:style('#kit-osk', 'left', ('%dpx'):format(math.floor((cfg.x or ((l + rr - r.w) / 2)) + 0.5)))
            doc:style('#kit-osk', 'top', ('%dpx'):format(math.floor((cfg.y or (b - r.h - 70)) + 0.5)))
        end
        doc:draw({layer = 'top', modal = cfg.modal ~= false})
    end
    return kb
end

-- kit.field{id, value, max, digits, placeholder, tip, class} -> markup: a text field with its keyboard button (id `<id>-kbd`, a keyboard icon next to it, shown only
-- while the controller is in use). Give the document to `kit.field_attach(kb, doc, {id, title, max, digits, on_change})` once it is loaded: the button, and
-- a click on the field while the button shows, open `kb` (the function returned opens it by hand) (the function returned opens it by hand) for the field's own text, and every key writes it back (and calls `on_change(text)`).
function K.field(o)
    local id = o.id
    return '<div class="kit-field"><input' .. attrs({{'type', 'text'}, {'id', id}, {'class', classes('field grow', o.class)}, {'value', o.value}, {'maxlength', o.max},
        {'placeholder', o.placeholder}, {'title', o.tip}}) .. '/>'
        .. '<button' .. attrs({{'id', id .. '-kbd'}, {'class', 'ibtn kit-kbd'}, {'title', o.digits and 'On-screen number pad' or 'On-screen keyboard'}}) .. '><span class="padkey a"></span>' .. K.icon('keyboard') .. '</button></div>'
end

function K.field_attach(kb, d, o)
    local function open()
        local el = d:query('#' .. o.id)
        if not el then return end
        kb:toggle{title = o.title, digits = o.digits, max = o.max, text = el:value() or '', on_close = o.on_close,
            get = function() local e = d:query('#' .. o.id) return e and e:value() end,
            on_change = function(t)
                local e = d:query('#' .. o.id)
                if e then e:value(t) end
                if o.on_change then o.on_change(t) end
            end}
    end
    kb:attach(d, {field = '#' .. o.id, button = '#' .. o.id .. '-kbd', open = open})
    return open
end

-- kit.confirm{title, text, yes = 'OK', no = 'Cancel', danger, on_result} -> dialog: a modal question in the middle of the screen. Call `dialog:draw()` from the
-- mod's draw handler for as long as `dialog:open()` is true; `on_result(true | false)` runs once (Esc and the controller's B answer no). The game and every
-- other overlay take no mouse and no pad meanwhile.
function K.confirm(o)
    local d = {}
    local doc, done
    local function finish(result)
        if done then return end
        done = true
        if o.on_result then o.on_result(result) end
        if doc then doc:close() end
    end
    function d:open() return not done end
    function d:cancel() finish(false) end
    function d:draw()
        if done then return end
        if not doc then
            local body = K.text(o.text or '') .. '<div style="height: 14px"></div>'
                .. K.buttons(K.button('kit-no', o.no or 'Cancel'), K.button('kit-yes', o.yes or 'OK', {primary = not o.danger, danger = o.danger}))
            local markup = K.page(K.window{id = 'kit-dialog', title = o.title or '', width = 380, body = body,
                class = 'kit-dialog', panel = false, x = 0, y = 0})
            local err
            doc, err = ui.rml.load_string(markup, {})
            if not doc then done = true; if o.on_result then o.on_result(false) end return end
            doc:on('#kit-yes', 'click', function() finish(true) end)
            doc:on('#kit-no', 'click', function() finish(false) end)
        end
        -- centred: the window's size is known once it is laid out
        local r = doc:query('#kit-dialog'):rect()
        if r then
            local l, t, rr, b = ui.screen()
            doc:style('#kit-dialog', 'left', ('%dpx'):format(math.floor((l + rr - r.w) / 2 + 0.5)))
            doc:style('#kit-dialog', 'top', ('%dpx'):format(math.floor((t + b - r.h) / 2 + 0.5)))
        end
        doc:draw({layer = 'top', modal = true})
    end
    return d
end

return K
