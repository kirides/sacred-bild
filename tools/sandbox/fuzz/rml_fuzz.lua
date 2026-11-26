-- Differential fuzzer of Sacred Gear's retained-mode RmlUi documents (docs/RmlRetained.md, docs/Sandbox.md).
--
-- Each case builds a random document (elements, classes, :hover / :active / :focus rules, transitions, animations, scroll boxes, images of
-- the game and of the mod, transforms, a data model with data-if / data-for / data-class / data-attr / data-value) and loads it twice:
-- A as every document runs (retained: updated, walked and rendered only when something changed) and B as its twin with rml_nocache
-- (updated, walked and rendered every frame, as before retained mode). Then random steps go to both alike: mouse moves, presses, wheel,
-- keys, text, clicks and nudges of nav stops, every setter native (class, attribute, style, value, markup, append, remove, show, focus,
-- blur, scroll, move, resize, model values), pad scrolling, clips and time. After each step both are updated and drawn, and must agree:
--   - the commands they recorded (A's retained list expanded; meshes compared by their vertices),
--   - the events they reported,
--   - the rows of rml_rects, rml_nav, rml_scrolls and rml_scrollbars (A handing in the same tables every frame, as kernel_ui does),
--   - and every few steps the pixels both draw (A through its retained vertex buffers, B command by command).
-- A difference is a source of change the retained mode misses (or a replay that draws differently).
--
-- gear_sandbox --hidden --no-mods --mods tools\sandbox\fuzz\mods --script tools\sandbox\fuzz\rml_fuzz.lua [seed=N] [cases=N] [steps=N]
--     [case=K] [out=dir] [verbose=1] [stop=1]
-- `case=K` runs only case K of the seed (a repro). `dt=` the frame time (default 1/64: exact in floats, see sbx.dt). Exit code: the number of failing cases (0: all agree).

local opts = {seed = 1, cases = 20, steps = 150, out = nil, verbose = false, stop = false, hide = 0, flicker = false, dt = 1 / 64}
for _, a in ipairs(arg or {}) do
    local k, v = a:match('^(%w+)=(.*)$')
    if k then opts[k] = tonumber(v) or v end
end
opts.verbose = opts.verbose == 1 or opts.verbose == '1'
opts.stop = opts.stop == 1 or opts.stop == '1'
opts.flicker = opts.flicker == 1 or opts.flicker == '1'
local flickers = 0      -- differences where the twin itself flickered (not counted as failures without flicker=1)
local lags = 0          -- differences A caught up with within 2 frames (not counted without strict=1)
opts.strict = opts.strict == 1 or opts.strict == '1'

local MOD = 'fuzz'
local W, H = sbx.size()
local HUD = 1

local rnd = math.random
local function pick(t) return t[rnd(#t)] end
local function chance(p) return rnd() < p end

---------------------------------------------------------------------------------------------------------------
-- Documents
---------------------------------------------------------------------------------------------------------------

local CLASSES = {'c0', 'c1', 'c2', 'c3', 'c4'}
local COLORS = {'#e0c040', '#4080e0c0', '#20202080', '#c04040', '#40c060', '#ffffff', '#00000000', '#8040c0'}
local IMAGES = {'gear:image/54', 'gear:image/281', 'gear:image/280', 'gear:image/318', 'gear:image/272', 'gear:image/323',
                'checker.png', 'gear:texture/GUI_MAIN_12.TGA', 'gear:image/14?point', 'gear:image/281?flip=h'}

local function color() return pick(COLORS) end

-- One random declaration block.
local function decls(depth)
    local d = {}
    local function add(s) d[#d + 1] = s end
    if chance(0.5) then add('background-color: ' .. color() .. ';') end
    if chance(0.4) then add('color: ' .. color() .. ';') end
    if chance(0.3) then add(('width: %dpx;'):format(rnd(20, 300))) end
    if chance(0.3) then add(('height: %dpx;'):format(rnd(10, 120))) end
    if chance(0.3) then add(('padding: %dpx;'):format(rnd(0, 12))) end
    if chance(0.2) then add(('margin: %dpx;'):format(rnd(0, 10))) end
    if chance(0.2) then add(('border: %dpx %s;'):format(rnd(1, 4), color())) end
    if chance(0.15) then add(('border-radius: %dpx;'):format(rnd(1, 12))) end
    if chance(0.2) then add(('font-size: %dpx;'):format(rnd(10, 28))) end
    if chance(0.15) then add('display: ' .. pick({'block', 'inline-block', 'flex', 'none'}) .. ';') end
    if chance(0.15) then add('opacity: ' .. pick({'0.3', '0.6', '1'}) .. ';') end
    if chance(0.15) then add('decorator: image(' .. pick(IMAGES) .. pick({'', ' repeat-x', ' contain', ' scale-none left center'}) .. ');') end
    if chance(0.1) then add(('transform: rotate(%ddeg) scale(%.2f);'):format(rnd(-30, 30), 0.8 + rnd() * 0.5)) end
    if chance(0.12) then add('transition: background-color color opacity ' .. pick({'0.1s', '0.25s', '0.6s'}) .. ' linear-in-out;') end
    if chance(0.06) then add('animation: ' .. pick({'0.5s', '1s', '2s'}) .. ' ' .. pick({'1', '3', 'infinite'}) .. ' pulse;') end
    if chance(0.1) then add('visibility: ' .. pick({'visible', 'hidden'}) .. ';') end
    return table.concat(d, ' ')
end

local function stylesheet()
    local rules = {
        'body { width: 100%; height: 100%; font-family: "Sacred Game"; font-size: 16px; color: #f0e0b0; }',
        'div, p, ul, li { display: block; }',
        '@keyframes pulse { from { opacity: 1; } to { opacity: 0.2; } }',
        '.scroll { overflow: auto; height: 90px; }',
        'scrollbarvertical { width: 10px; } scrollbarvertical slidertrack { background-color: #202020; } scrollbarvertical sliderbar { background-color: #a0a0a0; min-height: 12px; }',
        'input.text { width: 140px; height: 22px; background-color: #303030; }',
        'input.range { width: 160px; height: 16px; } input.range slidertrack { background-color: #404040; height: 6px; } input.range sliderbar { width: 10px; height: 16px; background-color: #e0c040; }',
        'input.checkbox { width: 16px; height: 16px; background-color: #505050; } input.checkbox:checked { background-color: #40c060; }',
        'button { display: inline-block; padding: 4px; background-color: #404860; }',
        'img { display: inline-block; }',
    }
    for _, c in ipairs(CLASSES) do
        if chance(0.8) then rules[#rules + 1] = '.' .. c .. ' { ' .. decls() .. ' }' end
        if chance(0.5) then rules[#rules + 1] = '.' .. c .. ':hover { ' .. decls() .. ' }' end
        if chance(0.3) then rules[#rules + 1] = '.' .. c .. ':active { ' .. decls() .. ' }' end
        if chance(0.2) then rules[#rules + 1] = '.' .. c .. ':focus { ' .. decls() .. ' }' end
        if chance(0.2) then rules[#rules + 1] = '.' .. pick(CLASSES) .. ' .' .. c .. ' { ' .. decls() .. ' }' end
    end
    if chance(0.4) then rules[#rules + 1] = 'button:hover { background-color: ' .. color() .. '; }' end
    if chance(0.3) then rules[#rules + 1] = '[data-x] { border: 2px ' .. color() .. '; }' end
    return table.concat(rules, '\n')
end

local ids = {}          -- the element ids of the current case

local function attrs(id)
    local a = {('id="%s"'):format(id)}
    if chance(0.6) then
        local cs = {}
        for _ = 1, rnd(1, 2) do cs[#cs + 1] = pick(CLASSES) end
        a[#a + 1] = ('class="%s"'):format(table.concat(cs, ' '))
    end
    if chance(0.15) then a[#a + 1] = ('style="%s"'):format(decls()) end
    if chance(0.1) then a[#a + 1] = 'data-x="1"' end
    return table.concat(a, ' ')
end

local MODEL_TEXT = {'alpha', 'beta', 'gamma', '', 'a much longer text that wraps around', '42'}

local function element(depth)
    local id = 'e' .. (#ids + 1)
    ids[#ids + 1] = id
    local k = rnd(100)
    if depth > 3 or k <= 18 then
        local t = {'text ' .. id, '{{ title }}', '{{ count }} items', pick(MODEL_TEXT)}
        return ('<span %s>%s</span>'):format(attrs(id), pick(t))
    elseif k <= 26 then
        return ('<img %s src="%s"/>'):format(attrs(id), pick(IMAGES))
    elseif k <= 36 then
        return ('<button %s>%s</button>'):format(attrs(id), pick({'OK', 'Cancel', '{{ title }}', 'B' .. id}))
    elseif k <= 42 then
        return ('<input type="text" class="text" %s value="%s"/>'):format(attrs(id):gsub('class="', 'class="text '), pick(MODEL_TEXT))
    elseif k <= 47 then
        return ('<input type="range" class="range" min="0" max="10" step="1" %s value="%d"/>'):format(attrs(id):gsub('class="', 'class="range '), rnd(0, 10))
    elseif k <= 51 then
        return ('<input type="checkbox" class="checkbox" %s/>'):format(attrs(id):gsub('class="', 'class="checkbox '))
    elseif k <= 56 then
        return ('<div %s data-if="flag">%s</div>'):format(attrs(id), element(depth + 1))
    elseif k <= 60 then
        return ('<div %s data-class-c1="flag" data-attr-title="title">{{ title }}</div>'):format(attrs(id))
    elseif k <= 65 then
        local inner = {}
        for i = 1, rnd(1, 3) do inner[#inner + 1] = element(depth + 1) end
        return ('<div %s><p data-for="item : items">{{ item }} <button>x</button></p>%s</div>'):format(attrs(id), table.concat(inner))
    elseif k <= 72 then
        local inner = {}
        for i = 1, rnd(3, 8) do inner[#inner + 1] = element(depth + 1) end
        return ('<div %s class="scroll" data-nav-scroll="1">%s</div>'):format(attrs(id):gsub('class="[^"]*"', ''), table.concat(inner))
    else
        local inner = {}
        for i = 1, rnd(1, 4) do inner[#inner + 1] = element(depth + 1) end
        return ('<div %s>%s</div>'):format(attrs(id), table.concat(inner))
    end
end

local function document()
    ids = {}
    local windows = {}
    for w = 1, rnd(1, 3) do
        local inner = {}
        for i = 1, rnd(2, 6) do inner[#inner + 1] = element(1) end
        local id = 'w' .. w
        windows[#windows + 1] = ('<div id="%s" data-panel="%s" style="position: absolute; left: %dpx; top: %dpx; width: %dpx; %s">%s</div>'):format(
            id, id, rnd(0, W - 300), rnd(0, H - 200), rnd(150, 420), decls(), table.concat(inner))
    end
    return ('<rml><head><style>%s</style></head><body data-model="model">%s</body></rml>'):format(stylesheet(), table.concat(windows))
end

local function model()
    return {title = pick(MODEL_TEXT), count = rnd(0, 9), flag = chance(0.5), items = {'one', 'two'}}
end

---------------------------------------------------------------------------------------------------------------
-- Comparing the twins
---------------------------------------------------------------------------------------------------------------

local function deep_equal(a, b, path)
    if type(a) ~= type(b) then return false, path .. ': ' .. type(a) .. ' vs ' .. type(b) end
    if type(a) == 'number' then
        -- layout floats of two documents laid out along different update paths differ in the last bits
        if math.abs(a - b) > 1e-3 then return false, path .. ': ' .. tostring(a) .. ' vs ' .. tostring(b) end
        return true
    end
    if type(a) ~= 'table' then
        if a ~= b then return false, path .. ': ' .. tostring(a) .. ' vs ' .. tostring(b) end
        return true
    end
    for k, v in pairs(a) do
        if k ~= 'listener' then
            local ok, why = deep_equal(v, b[k], path .. '.' .. tostring(k))
            if not ok then return false, why end
        end
    end
    for k in pairs(b) do
        if k ~= 'listener' and a[k] == nil then return false, path .. '.' .. tostring(k) .. ': missing in A' end
    end
    return true
end

local Twin = {}
Twin.__index = Twin

local function load_twin(markup, m)
    local calls = {'pressed'}
    local a, err = rml.rml_load_string(MOD, markup, false, m, calls)
    if not a then return nil, err end
    local b, err2 = rml.rml_load_string(MOD, markup, false, m, calls)
    if not b then rml.rml_close(a) return nil, err2 end
    rml.rml_nocache(b, true)
    return setmetatable({a = a, b = b, rows = {rects = {}, nav = {}, scrolls = {}, bars = {}}, mx = -100, my = -100, hover = false,
                         layer = HUD}, Twin)
end

function Twin:close()
    rml.rml_close(self.a)
    rml.rml_close(self.b)
end

-- Calls native `name` on both with the same arguments; their results must agree.
function Twin:both(name, ...)
    local ra = {pcall(rml[name], self.a, ...)}
    local rb = {pcall(rml[name], self.b, ...)}
    if ra[1] ~= rb[1] then
        return false, ('%s: A %s, B %s'):format(name, tostring(ra[2]), tostring(rb[2]))
    end
    -- handles (rml_query / rml_append / rml_relatives) are per document: compare their count only
    if name == 'rml_query' or name == 'rml_append' or name == 'rml_relatives' then
        local na = type(ra[2]) == 'table' and #ra[2] or (ra[2] and 1 or 0)
        local nb = type(rb[2]) == 'table' and #rb[2] or (rb[2] and 1 or 0)
        if na ~= nb then return false, ('%s: A found %d, B %d'):format(name, na, nb) end
        return true
    end
    local ok, why = deep_equal({unpack(ra, 2)}, {unpack(rb, 2)}, name)
    return ok, why
end

-- One frame of both: update (events), walks, draw; compares all of it. `pixels`: also the pixels. B's walks and draw are kept in self.b_last
-- (the previous frame's in self.b_prev), so that settle() can tell a twin that flickers from one that is stable.
function Twin:frame(step, pixels, prefix)
    local ea = rml.rml_update(self.a, self.mx, self.my, self.hover)
    local eb = rml.rml_update(self.b, self.mx, self.my, self.hover)
    local fail
    local ok, why = deep_equal(ea, eb, 'events')
    if not ok then fail = why end
    local brows = {}
    for _, walk in ipairs({{'rml_rects', 'rects'}, {'rml_nav', 'nav'}, {'rml_scrolls', 'scrolls'}, {'rml_scrollbars', 'bars'}}) do
        local fn, key = walk[1], walk[2]
        local ra = rml[fn](self.a, self.rows[key])
        self.rows[key] = ra
        local rb = rml[fn](self.b, {})
        brows[key] = rb
        ok, why = deep_equal(ra, rb, fn)
        if not ok and not fail then fail = why end
    end
    sbx.capture_begin()
    local na = rml.rml_draw(self.a, self.layer, -1)
    local ca = sbx.capture_end()
    sbx.capture_begin()
    local nb = rml.rml_draw(self.b, self.layer, -1)
    local cb = sbx.capture_end()
    self.b_prev, self.b_last = self.b_last, {rows = brows, cap = cb}
    sbx.commit()        -- ends the frame's recording (Ui keeps at most 16384 commands per frame)
    if not fail then
        ok, why = sbx.compare(ca, cb)
        if not ok and why:match('^command %d+: image%s') and sbx.render_compare(ca, cb) == 0 then
            -- Another picture with the same pixels: a font atlas the twin's render rebuilt in this frame (A's recorded draw still names the old
            -- one, which is released only after the frames in flight, and A draws again from the next frame on).
            ok = true
            self.atlas = (self.atlas or 0) + 1
        end
        if not ok then
            if prefix then sbx.render_compare(ca, cb, prefix .. '_fail') end
            fail = 'draw: ' .. why
        elseif na ~= nb then
            fail = ('draw count: A %d, B %d'):format(na, nb)
        end
    end
    if opts.verbose then
        local st = rml.rml_stats(self.a)
        local last = self.last_stats or {updates = 0, renders = 0}
        sbx.log(('     A updated %d rendered %d (t %.3f, next %.3f)'):format(st.updates - last.updates, st.renders - last.renders, sbx.time(), st.next_update))
        self.last_stats = st
    end
    if not fail and pixels then
        local diff, largest = sbx.render_compare(ca, cb, prefix)
        if diff > 0 then fail = ('pixels: %d differ (largest channel difference %d)'):format(diff, largest) end
    end
    if fail then return false, fail end
    return true
end

-- After a difference: idle frames (no input) until the twins agree again. -> 'lag', frames (A caught up: it showed the change late),
-- 'flicker' (B itself changes from frame to frame: a document whose hover or layout never settles; retained A stays on one state, B
-- flickers as before retained mode), or 'stale' (B is stable and A stays different: a missed change).
function Twin:settle()
    local frames, agree, changed = 8, {}, {}
    for k = 1, frames do
        sbx.advance(opts.dt)
        agree[k] = self:frame(-k, false, nil) and true or false
        if self.b_prev then
            local same_rows = deep_equal(self.b_prev.rows, self.b_last.rows, 'b')
            local same_draw = sbx.compare(self.b_prev.cap, self.b_last.cap)
            changed[k] = not (same_rows and same_draw)
        end
    end
    sbx.capture_clear()
    self.b_prev, self.b_last = nil, nil
    -- B changing in the second half means it never settles
    for k = frames / 2, frames do
        if changed[k] then return 'flicker' end
    end
    local first = frames + 1
    for k = frames, 1, -1 do
        if not agree[k] then break end
        first = k
    end
    if first <= frames then return 'lag', first end
    return 'stale'
end

---------------------------------------------------------------------------------------------------------------
-- Steps: plain data ({native = name, args = {...}}, {mouse = {x, y, hover}}, {time = s}, {layer = n}; `clip` on any), so that a failing
-- case can be replayed with fewer of them (shrinking, below).
---------------------------------------------------------------------------------------------------------------

local function some_id() return '#' .. pick(ids) end
local function some_selector()
    local k = rnd(10)
    if k <= 6 then return some_id() end
    if k <= 8 then return '.' .. pick(CLASSES) end
    return pick({'button', 'span', 'img', 'input', 'div', 'p', '#w1', '#w2'})
end

local STYLE_PROPS = {
    {'background-color', function() return color() end},
    {'color', function() return color() end},
    {'width', function() return rnd(10, 300) .. 'px' end},
    {'height', function() return rnd(10, 150) .. 'px' end},
    {'left', function() return rnd(0, W - 100) .. 'px' end},
    {'display', function() return pick({'block', 'none', 'inline-block'}) end},
    {'opacity', function() return pick({'0', '0.5', '1'}) end},
    {'transform', function() return ('rotate(%ddeg)'):format(rnd(-45, 45)) end},
    {'decorator', function() return 'image(' .. pick(IMAGES) .. ')' end},
    {'font-size', function() return rnd(8, 30) .. 'px' end},
    {'visibility', function() return pick({'visible', 'hidden'}) end},
}

local VKEYS = {0x08, 0x2E, 0x0D, 0x09, 0x1B, 0x25, 0x26, 0x27, 0x28, 0x24, 0x23, 0x41}

local function N(name, ...) return {native = name, args = {...}, n = select('#', ...)} end

-- One random step for the twin as it is now (the mouse goes to its nav stops sometimes).
local function gen_step(t)
    local k = rnd(100)
    if k <= 22 then
        local x, y = t.mx, t.my
        local nav = t.rows.nav
        if chance(0.5) and nav and #nav > 0 then
            local r = pick(nav)
            x, y = math.floor(r.x + rnd() * r.w), math.floor(r.y + rnd() * r.h)
        elseif chance(0.8) then
            x, y = rnd(0, W - 1), rnd(0, H - 1)
        end
        return {mouse = {x, y, chance(0.85)}}
    elseif k <= 30 then
        local kind = pick({'down', 'up'})
        if chance(0.5) then return N('rml_mouse', kind, t.mx, t.my) end
        return N('rml_mouse', kind)
    elseif k <= 33 then return N('rml_wheel', pick({-3, -1, 1, 2}))
    elseif k <= 37 then return N('rml_key', pick(VKEYS), chance(0.6))
    elseif k <= 40 then return N('rml_text', pick({'a', 'xyz', ' ', 'Ä'}))
    elseif k <= 44 then return N('rml_click', rnd(1, math.max(1, #(t.rows.nav or {}))))
    elseif k <= 46 then return N('rml_nudge', rnd(1, math.max(1, #(t.rows.nav or {}))), pick({-2, -1, 1, 3}))
    elseif k <= 53 then return N('rml_class', some_selector(), pick(CLASSES), chance(0.5))
    elseif k <= 58 then
        local p = pick(STYLE_PROPS)
        return N('rml_style', some_selector(), p[1], chance(0.2) and false or p[2]())
    elseif k <= 62 then
        return N('rml_attr', some_selector(), pick({'data-x', 'title', 'disabled', 'src', 'value', 'checked'}),
                 chance(0.3) and false or pick({'1', 'abc', pick(IMAGES), ''}))
    elseif k <= 65 then return N('rml_value', some_selector(), pick({'7', 'hello', '', '3'}))
    elseif k <= 68 then
        return N('rml_html', some_id(), pick({'new <b>text</b>', '<button class="c2">N</button>', '', '<img src="' .. pick(IMAGES) .. '"/>', 'same'}))
    elseif k <= 70 then
        local tag = pick({'div', 'button', 'span', 'img'})
        local a = {class = pick(CLASSES)}
        if tag == 'img' then a.src = pick(IMAGES) end
        return N('rml_append', some_id(), tag, a)
    elseif k <= 72 then return N('rml_remove', some_id())
    elseif k <= 74 then
        -- hide=1: also hide documents. Known difference: RmlUi asks for no update of a hidden document, so its animations stand still
        -- while the twin's run on (docs/Sandbox.md, "Findings").
        return N('rml_show', opts.hide == 0 or chance(0.75))
    elseif k <= 77 then return N('rml_focus', some_selector(), chance(0.7))
    elseif k <= 78 then return N('rml_blur')
    elseif k <= 81 then
        if chance(0.8) then return N('rml_scroll', some_selector(), rnd(0, 200)) end
        return N('rml_scroll', some_selector())
    elseif k <= 83 then return N('rml_move', '#w' .. rnd(1, 3), rnd(0, W - 200), rnd(0, H - 100))
    elseif k <= 84 then return N('rml_resize', '#w' .. rnd(1, 3), rnd(80, 500), rnd(40, 400))
    elseif k <= 90 then
        local key = pick({'title', 'count', 'flag', 'items'})
        local v
        if key == 'title' then v = pick(MODEL_TEXT) elseif key == 'count' then v = rnd(0, 9) elseif key == 'flag' then v = chance(0.5)
        else
            v = {}
            for i = 1, rnd(0, 4) do v[i] = pick(MODEL_TEXT) end
        end
        return N('rml_model_set', key, v)
    elseif k <= 91 then return N('rml_model_changed')
    elseif k <= 93 then return N('rml_pad_scroll', t.mx, t.my, pick({-1, 1}))
    elseif k <= 94 then return N('rml_pad_reveal', t.mx, t.my)
    elseif k <= 95 then return N('rml_query', some_selector(), true)
    elseif k <= 96 then return N('rml_rect', some_selector())
    elseif k <= 97 then return {layer = pick({1, 2})}
    else return {time = pick({0.0625, 0.25, 0.5, 1.25, 3})}     -- exact in floats (see sbx.dt)
    end
end

local function show(v)
    if type(v) == 'table' then
        local parts = {}
        for k2, x in pairs(v) do parts[#parts + 1] = (type(k2) == 'number' and '' or (k2 .. '=')) .. show(x) end
        return '{' .. table.concat(parts, ',') .. '}'
    elseif type(v) == 'string' then return ('%q'):format(v)
    end
    return tostring(v)
end

local function describe(st)
    local s = '?'
    if st.native then
        local a = {}
        for i = 1, st.n do a[i] = show(st.args[i]) end
        s = st.native .. '(' .. table.concat(a, ', ') .. ')'
    elseif st.mouse then s = ('mouse %d,%d hover %s'):format(st.mouse[1], st.mouse[2], tostring(st.mouse[3]))
    elseif st.time then s = ('time +%.2f'):format(st.time)
    elseif st.layer then s = 'layer ' .. st.layer
    end
    if st.clip then s = s .. (' [clip %d,%d %dx%d]'):format(st.clip[1], st.clip[2], st.clip[3], st.clip[4]) end
    return s
end

local function exec(t, st)
    if st.mouse then
        t.mx, t.my, t.hover = st.mouse[1], st.mouse[2], st.mouse[3]
        return true
    elseif st.time then
        sbx.advance(st.time)
        return true
    elseif st.layer then
        t.layer = st.layer
        return true
    end
    return t:both(st.native, unpack(st.args, 1, st.n))
end

---------------------------------------------------------------------------------------------------------------
-- Cases
---------------------------------------------------------------------------------------------------------------

-- Runs `steps` on a fresh twin of `markup`, or generates them as it goes when `steps` is nil. Pixels are compared every `pixel_every`
-- steps (nil: never). -> ok (nil: the document did not load), why, the steps up to the failing one, the failing step's index
local function play(markup, m, steps, prefix, pixel_every)
    local t, err = load_twin(markup, m)
    if not t then return nil, err end
    local generate = steps == nil
    steps = steps or {}
    local ok, why = t:frame(0, pixel_every ~= nil, prefix)
    local n = generate and opts.steps or #steps
    local failed_at = (not ok) and 0 or nil
    local kind, lag
    for s = 1, n do
        if not ok then break end
        local st = steps[s]
        if generate then
            st = gen_step(t)
            if chance(0.05) then st.clip = {rnd(0, W / 2), rnd(0, H / 2), rnd(100, W), rnd(100, H)} end
            steps[s] = st
        end
        if opts.verbose then sbx.log(('  %d: %s'):format(s, describe(st))) end
        local good, w = exec(t, st)
        if not good then
            ok, why, failed_at = false, 'native result: ' .. tostring(w), s
            break
        end
        if st.clip then sbx.clip(true, st.clip[1], st.clip[2], st.clip[3], st.clip[4]) end
        sbx.advance(opts.dt)
        ok, why = t:frame(s, pixel_every ~= nil and (s % pixel_every == 0 or s == n), prefix)
        if st.clip then sbx.clip(false) end
        sbx.capture_clear()
        t.b_prev, t.b_last = nil, nil
        if not ok then
            failed_at = s
            kind, lag = t:settle()
            if kind == 'flicker' and generate then flickers = flickers + 1 end
            if kind == 'lag' and lag <= 2 and generate then lags = lags + 1 end
            if generate and ((kind == 'flicker' and not opts.flicker) or (kind == 'lag' and lag <= 2 and not opts.strict)) then
                -- not retained mode's doing (flicker, see settle), or by design: the hover of an element that moved under the still mouse
                -- settles one update later (docs/RmlRetained.md, principle 4); the twin, which moves the mouse before every update, a frame
                -- earlier. The rest of the case would only differ on.
                ok, failed_at = true, nil
                break
            end
        end
    end
    t:close()
    sbx.capture_clear()
    if generate and failed_at then
        for i = #steps, failed_at + 1, -1 do steps[i] = nil end
    end
    return ok, why, steps, failed_at, kind, lag
end

-- Shrinks a failing step list (ddmin: drops chunks, then single steps, while it still fails).
local function shrink(markup, m, steps, want)
    local function fails(list)
        local ok, _, _, _, kind = play(markup, m, list, nil, 10)
        return ok == false and kind == want
    end
    local n, budget = 2, 600
    while #steps >= 2 and budget > 0 do
        local size = math.ceil(#steps / n)
        local reduced = false
        for start = 1, #steps, size do
            budget = budget - 1
            local candidate = {}
            for i = 1, #steps do
                if i < start or i >= start + size then candidate[#candidate + 1] = steps[i] end
            end
            if #candidate > 0 and fails(candidate) then
                steps = candidate
                n = math.max(n - 1, 2)
                reduced = true
                break
            end
        end
        if not reduced then
            if size == 1 then break end
            n = math.min(n * 2, #steps)
        end
    end
    return steps
end

-- A step list as Lua source (replay=<file> plays it again).
local function serialize(markup, m, steps)
    local out = {'return {markup = ' .. ('%q'):format(markup) .. ',', 'model = ' .. show(m) .. ',', 'steps = {'}
    for _, st in ipairs(steps) do
        local parts = {}
        if st.native then
            local a = {}
            for i = 1, st.n do a[i] = show(st.args[i]) end
            parts[#parts + 1] = ('native = %q, n = %d, args = {%s}'):format(st.native, st.n, table.concat(a, ', '))
        elseif st.mouse then parts[#parts + 1] = ('mouse = {%d, %d, %s}'):format(st.mouse[1], st.mouse[2], tostring(st.mouse[3]))
        elseif st.time then parts[#parts + 1] = 'time = ' .. st.time
        elseif st.layer then parts[#parts + 1] = 'layer = ' .. st.layer
        end
        if st.clip then parts[#parts + 1] = ('clip = {%d, %d, %d, %d}'):format(st.clip[1], st.clip[2], st.clip[3], st.clip[4]) end
        out[#out + 1] = '  {' .. table.concat(parts, ', ') .. '},'
    end
    out[#out + 1] = '}}'
    return table.concat(out, string.char(10))
end

local function run_case(seed, case)
    math.randomseed(seed * 100003 + case)
    local markup, m = document(), model()
    local prefix = opts.out and (opts.out .. ('/seed%d_case%d'):format(seed, case)) or nil
    local ok, why, steps, at, kind, lag = play(markup, m, nil, prefix, 10)
    if ok == nil then
        sbx.log(('case %d: the document did not load (%s): skipped'):format(case, tostring(why)))
        return true
    end
    if ok then return true end
    sbx.log(('FAIL seed %d case %d at step %d (%s%s): %s'):format(seed, case, at, kind, lag and (' ' .. lag .. ' frames') or '', why))
    local small = shrink(markup, m, steps, kind)
    local ok2, why2 = play(markup, m, small, prefix and (prefix .. '_min') or nil, 1)
    local lines = {}
    for i, st in ipairs(small) do lines[i] = ('%d. %s'):format(i, describe(st)) end
    sbx.log(('  shrunk to %d steps%s:'):format(#small, ok2 == false and '' or ' (passes when replayed alone)'))
    for _, l in ipairs(lines) do sbx.log('    ' .. l) end
    if ok2 == false then sbx.log('  -> ' .. why2) end
    if opts.out then
        local f = io.open(opts.out .. ('/seed%d_case%d.rml'):format(seed, case), 'w')
        if f then f:write(markup) f:close() end
        f = io.open(opts.out .. ('/seed%d_case%d.repro.lua'):format(seed, case), 'w')
        if f then f:write(serialize(markup, m, small)) f:close() end
        f = io.open(opts.out .. ('/seed%d_case%d.steps.txt'):format(seed, case), 'w')
        if f then f:write(why, '\n\nshrunk:\n', table.concat(lines, '\n'), '\n', tostring(why2), '\n') f:close() end
    end
    return false
end

if opts.replay then
    -- replay=<file.repro.lua>: the shrunk steps of a failure, verbose (verbose=1 also lists A's updates and renders per frame)
    local r = dofile(opts.replay)
    local ok, why, _, at, kind, lag = play(r.markup, r.model, r.steps, opts.out and (opts.out .. '/replay') or nil, 1)
    sbx.log(ok and 'replay: the twins agree' or ('replay: step %s (%s%s): %s'):format(tostring(at), tostring(kind), lag and (' ' .. lag) or '', why))
    return ok and 0 or 1
end

local failures = 0
local first, last = 1, opts.cases
if opts.case then first, last = opts.case, opts.case end
for case = first, last do
    local ok = run_case(opts.seed, case)
    if not ok then
        failures = failures + 1
        if opts.stop then break end
    elseif opts.verbose then
        sbx.log(('case %d ok'):format(case))
    end
end
sbx.log(('rml_fuzz seed %d: %d of %d cases failed (not counted: %d flicker, %d lag of 1-2 frames)'):format(opts.seed, failures, last - first + 1, flickers, lags))
return failures
