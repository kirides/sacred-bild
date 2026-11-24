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
function K.window(o)
    local style = {}
    if o.x or o.y then style[#style + 1] = 'position: absolute; left: ' .. num(o.x or 0) .. 'px; top: ' .. num(o.y or 0) .. 'px;' end
    if o.width then style[#style + 1] = 'width: ' .. num(o.width) .. 'px;' end
    if o.height then style[#style + 1] = 'height: ' .. num(o.height) .. 'px;' end
    local close = o.close and ('<button id="' .. esc(o.close) .. '" class="close act" data-no-padkey="" style="float: right; min-width: 0; padding: 0 8px; margin: 0;">x</button>') or ''
    return '<div' .. attrs({{'id', o.id}, {'class', classes('window', o.class)}, {'data-panel', o.panel}, {'data-title', o.panel and (o.title or o.panel)},
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
    return '<rml><head>' .. ((o and o.head) or '') .. '</head><body>' .. markup .. '</body></rml>'
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
                class = 'kit-dialog', x = 0, y = 0})
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
