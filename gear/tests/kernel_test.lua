-- Tests of the Lua kernel (gear/lua/kernel.lua) outside the game: the native side is mocked, the mods are in
-- memory. Run with LuaJIT from the repository root:
--     luajit gear/tests/kernel_test.lua
-- (vcpkg puts luajit.exe under vcpkg_installed\x86-windows-static\tools\luajit). Exit code 0: all passed.

local kernel_path = arg and arg[1] or 'gear/lua/kernel.lua'

local failures, passes = 0, 0
local function ok(cond, name)
    if cond then passes = passes + 1 else failures = failures + 1; print('FAIL ' .. name) end
end

-- Mod files: dir -> name -> text
local files = {}
local logs = {}
local clock = 0
local hero_id = nil
local objects = {}
local pipe_events = {}
local sent = {}
local ui_recording, ui_published = {}, {}
local pickup_published
ui_events_queue, keys_held, ui_mouse_at = {}, {}, {0, 0}
layout_file, moved_windows = '{"game:UI_WND_INVENTORY":{"x":2048,"y":1024}}', {}

local native = {
    version = 'test',
    log = function(s) logs[#logs + 1] = s end,
    mod_dirs = function()
        local t = {}
        for d in pairs(files) do t[#t + 1] = d end
        table.sort(t)
        return t
    end,
    read_file = function(dir, rel)
        local f = files[dir] and files[dir][rel]
        if f then return f end
        return nil, 'cannot read the file'
    end,
    load = function(text, name, env)
        local chunk, err = loadstring(text, name)
        if not chunk then return nil, err end
        setfenv(chunk, env)
        return chunk
    end,
    time = function() return clock end,
    reset_budget = function() end,
    table_info = function(name)
        if name == 'object_types' then return 4, 0x80 end
        if name == 'creature_types' then return 2, 0x56 end
    end,
    table_read = function(name, index, offset, kind)
        if name == 'object_types' and offset == 0x2E then return index == 2 and 3 or 8 end
        return 0
    end,
    table_string = function(name, index, offset)
        if name == 'object_types' and offset == 0x37 then
            return ({[1] = 'Sword', [2] = 'Goblin', [3] = ''})[index] or nil
        end
    end,
    hero = function() return hero_id end,
    world_changes = function() return hero_id ~= nil end,
    object = function(id) return objects[id] end,
    creatures_near = function() local t = {}; for id in pairs(objects) do t[#t + 1] = id end; table.sort(t); return t end,
    spawn = function(type, x, y, layer)
        local id = 100 + #sent
        objects[id] = {id = id, type = type, x = x, y = y, layer = layer, creature = true, hp = 10, max_hp = 10}
        return id
    end,
    teleport = function(id, x, y) objects[id].x, objects[id].y = x, y; return true end,
    set_health = function(id, hp) objects[id].hp = hp; return true end,
    set_gold = function(id, g) objects[id].gold = g; return true end,
    pipe_open = function(name) return 7 end,
    pipe_close = function() end,
    pipe_send = function(h, text) sent[#sent + 1] = text; return true end,
    pipe_poll = function(h)
        local e = pipe_events
        pipe_events = {}
        return e
    end,
    pipe_connected = function() return true end,
    -- UI drawing: the commands of the frame being recorded, then the published ones
    ui_rect = function(...) ui_recording[#ui_recording + 1] = {'rect', ...}; return true end,
    ui_text = function(...) ui_recording[#ui_recording + 1] = {'text', ...}; return true end,
    ui_image = function(...) ui_recording[#ui_recording + 1] = {'image', ...}; return true end,
    ui_commit = function() ui_published = ui_recording; ui_recording = {} end,
    ui_image_count = function() return 3 end,
    ui_image_info = function(id) if id > 0 and id < 3 then return {sheet = 'GUI_spell0' .. id .. '.TGA', width = 64, height = 64, loaded = true} end end,
    ui_font_count = function() return 8 end,
    ui_text_size = function(text) return #text * 7, 14 end,
    ui_windows = function() return {{name = 'UI_WND_INVENTORY', slot = 1, x = 0, y = 388, width = 640, height = 256, visible = true, drawn = true, movable = true}} end,
    ui_state = function() return {available = true, mode = 'game', mouse_x = ui_mouse_at[1], mouse_y = ui_mouse_at[2], screen_left = 0, screen_top = 0, screen_right = 1024, screen_bottom = 768} end,
    item = function(id)
        if id == 50 then return {id = 50, type = 2, kind = 5, flags = 0, type_flags = 0, on_ground = true, rarity = 13, set = false, classes = 0x8, gold = false, x = 1, y = 2, layer = 0} end
        if id == 51 then return {id = 51, type = 0x13C1, kind = 9, flags = 0, type_flags = 0x1000000, on_ground = true, rarity = 0, set = false, classes = 0xC0003F, gold = false, x = 1, y = 2, layer = 0} end
    end,
    items_near = function() return {50, 51} end,
    hero_class = function(id) if id == 1 then return 4, 0x8 end end,
    pickup_rules = function(b, d, a) pickup_published = {block = b, dim = d, alpha = a} end,
    pickup_refused = function() return 0 end,
    ui_region = function(...) ui_recording[#ui_recording + 1] = {'region', ...}; return true end,
    ui_events = function() local e = ui_events_queue; ui_events_queue = {}; return e end,
    key_down = function(vk) return keys_held[vk] == true end,
    layout_read = function() return layout_file end,
    layout_write = function(text) layout_file = text; return true end,
    ui_can_move_windows = function() return true end,
    ui_move_window = function(slot, x, y, set) moved_windows[slot] = set and {x, y} or nil; return true end,
    write_file = function(dir, rel, text) files[dir] = files[dir] or {}; files[dir][rel] = text; return true end,
    -- automation
    auto_instance = function() return 'test' end,
    auto_mods = function() return {} end,
    auto_state = function() return {menus = true, in_game = false, dialog = false, cinematic = false, width = 800, height = 600, mouse_x = 0, mouse_y = 0} end,
    auto_ui = function() return {} end,
    auto_move = function() end,
    auto_button = function() end,
    auto_key = function() end,
    auto_char = function() end,
    auto_release = function() end,
    auto_load = function() return true end,
    auto_screenshot = function() return true end,
    auto_write = function(name, text) files.__written = files.__written or {}; files.__written[name] = text; return true end,
    auto_quit = function(code) error('quit ' .. code) end,
}

files.a = {
    ['mod.lua'] = "return {id = 'a', version = '1', pipes = {'p1'}, config = {x = 1, y = 2}, automation = true}",
    ['config.lua'] = "return {y = 5}",
    ['main.lua'] = [[
        record = {}
        gear.on('load', function() record.loaded = true end)
        gear.on('frame', function(dt) record.frames = (record.frames or 0) + 1 end)
        gear.on('session.start', function(h) record.hero = h.id end)
        gear.on('session.stop', function() record.stopped = true end)
        gear.on('test.ping', function(v) record.ping = v end)
        record.cfg = gear.config
        record.json = gear.json.encode({a = {1, 2, 3}, b = 'x"y', c = true})
        local v, err = gear.json.decode('{"type":"chat","data":{"text":"#hi \\u00e9","n":[1,2.5,-3e2]},"z":null}')
        record.decoded = v
        record.bad = select(2, gear.json.decode('{"a":'))
        gear.after(1, function() record.after = true end)
        local n = 0
        local t = gear.every(0.5, function() n = n + 1; record.every = n end)
        gear.after(1.6, function() gear.cancel(t) end)
        local p = gear.pipe.open('p1')
        p.on_message = function(text) record.msg = text end
        p.on_connect = function() record.connected = true end
        record.pipe_denied = not pcall(gear.pipe.open, 'other')
        record.ro = not pcall(function() gear.data.object_types.find = nil end)
        record.emit_reserved = not pcall(gear.emit, 'session.start')
        record.auto = gear.auto ~= nil
    record.auto_instance = gear.auto and gear.auto.instance
        gear.task(function()
            record.task = 'started'
            gear.wait(1)
            record.task = 'waited'
            local got = gear.wait_until(function() return record.flag end, 5)
            record.task = got and 'flagged' or 'timeout'
        end)
        record.goblin = gear.data.object_types:find('goblin')
        record.search = #gear.data.object_types:search('o')
        gear.on('draw', function(state)
            record.draw_mode = state and state.mode
            gear.ui.rect(10, 20, 100, 50, gear.ui.rgba(0, 0, 0, 128))
            gear.ui.text('a\\b', 12, 22, {color = {255, 0, 0}, align = 'center', width = 96, layer = 'top', font = 'game1'})
            gear.ui.image(1, 0, 0, 32, 32, {fit = 'contain', window = 'ui_wnd_inventory'})
            record.no_window = gear.ui.image(1, 0, 0, 32, 32, {window = 'UI_WND_NOPE'})
            record.bad_layer = not pcall(gear.ui.rect, 0, 0, 1, 1, 0, {layer = 'nope'})
        end)
        record.images = #gear.ui.images('spell01')
        gear.on('frame', function()
            local h = gear.world.hero()
            if not h then return end
            record.class = h:class()
            local items = gear.world.items_near(0, 0, 500)
            record.items = items
            local block = {}
            for _, it in ipairs(items) do
                if not gear.world.usable_by(it, 'gladiator') then block[#block + 1] = it.id end
            end
            gear.world.pickup_rules({block = block, dim = block, dim_alpha = 80})
        end)
    ]],
}
files.b = {
    ['mod.lua'] = "return {id = 'b', requires = {'a'}}",
    ['main.lua'] = [[
        gear.on('load', function() gear.emit('test.ping', 42) end)
        gear.on('frame', function() error('boom') end)
        has_auto = gear.auto ~= nil
    ]],
}
files.w = {
    ['mod.lua'] = "return {id = 'w'}",
    ['main.lua'] = [[
        widgets = {checked = false, index = 1}
        gear.on('draw', function()
            local ui = gear.ui
            local ch, v = ui.checkbox('cb', 10, 10, 'Check', widgets.checked, {tooltip = 'tip'})
            if ch then widgets.checked = v; widgets.changes = (widgets.changes or 0) + 1 end
            local dc, i = ui.dropdown('dd', 10, 40, 100, {'one', 'two', 'three'}, widgets.index)
            if dc then widgets.index = i end
            widgets.ui = ui
            widgets.panel = ui.panel('p', 'Panel', 400, 400, 200, 100, {resizable = true, min_width = 150})
            widgets.panels = ui.panels()
        end)
    ]],
}
files.c = {    ['mod.lua'] = "return {id = 'c', requires = {'missing'}}",
    ['main.lua'] = "error('must not run')",
}

local f = assert(io.open(kernel_path, 'rb'))
local source = f:read('*a')
f:close()
local chunk = assert(loadstring(source, '=kernel'))
local kernel = chunk(native)

ok(kernel.load() == 3, 'three mods load (c is skipped)')

-- Reach mod a's globals through a handler's environment.
local env_a
for _, l in ipairs(logs) do
    if l:find('mod a 1 loaded') then env_a = true end
end
ok(env_a, 'mod a logged as loaded')

-- The mods' globals: find them through the functions they registered (getfenv of a handler).
local rec
do
    local i = 1
    while true do
        local name, value = debug.getupvalue(kernel.dispatch, i)
        if not name then break end
        if name == 'handlers' then
            for _, h in ipairs(value['frame']) do
                local env = getfenv(h.fn)
                if env.record then rec = env.record end
            end
        end
        i = i + 1
    end
end
ok(rec ~= nil, 'found mod a globals')
ok(rec.loaded, 'load event')
ok(rec.ping == 42, 'custom event from mod b reaches mod a')
ok(rec.cfg.x == 1 and rec.cfg.y == 5, 'config defaults and config.lua')
ok(rec.json == '{"a":[1,2,3],"b":"x\\"y","c":true}', 'json encode: ' .. tostring(rec.json))
ok(rec.decoded and rec.decoded.data.text == '#hi \195\169', 'json decode string with \\u escape')
ok(rec.decoded and rec.decoded.data.n[2] == 2.5 and rec.decoded.data.n[3] == -300, 'json decode numbers')
ok(rec.decoded and rec.decoded.z ~= nil, 'json null kept')
ok(rec.bad ~= nil, 'json decode error message')
ok(rec.pipe_denied, 'pipe not listed in mod.lua is refused')
ok(rec.ro, 'data is read only')
ok(rec.emit_reserved, 'reserved events cannot be emitted')
ok(rec.auto, 'automation for a mod that asks for it')
ok(rec.auto_instance == 'test', 'gear.auto.instance')
ok(rec.goblin == 2, 'type by name')
ok(rec.search == 2, 'type search')

-- Frames: session start, pipes, timers, tasks.
hero_id = 1
objects[1] = {id = 1, type = 5, x = 100, y = 200, layer = 0, creature = true, hp = 50, max_hp = 100, gold = 10, level = 3}
pipe_events = {{'connect', ''}, {'message', 'hello'}}
kernel.tick(0.016)
ok(rec.hero == 1, 'session.start with the hero')
ok(rec.connected and rec.msg == 'hello', 'pipe connect and message')
ok(rec.task == 'started', 'task runs until its first wait')
ok(rec.draw_mode == 'game', 'draw handlers get the UI state')
ok(#ui_published >= 3 and ui_published[1][1] == 'rect', 'a frame publishes its draws (' .. #ui_published .. ')')
ok(ui_published[1][1] == 'rect' and ui_published[1][8] == 0x80000000, 'rect with rgba color')
ok(ui_published[2][1] == 'text' and ui_published[2][2] == 2 and ui_published[2][4] == 'a/b' and
   ui_published[2][9] == 0xFFFF0000 and ui_published[2][10] == 1 and ui_published[2][11] == 1, 'text: layer, plain text, color table, align')
ok(ui_published[3][1] == 'image' and ui_published[3][3] == 1 and ui_published[3][10] == 1, 'image in a window, contain')
ok(rec.no_window == false and rec.bad_layer, 'unknown window draws nothing, bad layer errors')
ok(rec.images == 1, 'images by sheet')
ok(rec.class == 'darkelf', 'hero class (' .. tostring(rec.class) .. ')')
local it1, it2 = rec.items and rec.items[1], rec.items and rec.items[2]
ok(it1 and it1.category == 'weapon' and it1.quality == 'rare' and it1.classes[1] == 'darkelf' and #it1.classes == 1,
   'item: category, quality, classes')
ok(it2 and it2.category == 'potion' and it2.potion == 'health' and #it2.classes == 0 and it2.quest, 'potion, every class, quest')
ok(pickup_published and #pickup_published.block == 1 and pickup_published.block[1] == 50 and pickup_published.alpha == 80,
   'pickup rules published')
clock = 0.6
kernel.tick(0.016)
ok(rec.every == 1, 'every fires')
clock = 1.1
kernel.tick(0.016)
ok(rec.after and rec.every == 2, 'after fires, every again')
ok(rec.task == 'waited', 'task resumed after its wait')
clock = 1.7
kernel.tick(0.016)     -- every (due 1.5) and the cancel (due 1.6) both run
ok(rec.every == 3, 'every a third time (' .. tostring(rec.every) .. ')')
clock = 2.5
kernel.tick(0.016)
ok(rec.every == 3, 'cancelled timer stops (' .. tostring(rec.every) .. ')')
rec.flag = true
kernel.tick(0.016)
ok(rec.task == 'flagged', 'wait_until resumes on its condition')
for _ = 1, 12 do kernel.tick(0.016) end
local disabled
for _, l in ipairs(logs) do if l:find('%[b%] 10 errors') then disabled = true end end
ok(disabled, 'a mod with 10 errors is switched off')
hero_id = nil
kernel.tick(0.016)
ok(rec.stopped, 'session.stop')
kernel.idle()

-- Widgets: a region's handle from the published frame, a click on it (press and release), the next frame.
local function region_at(x, y)
    local last
    for _, c in ipairs(ui_published) do
        if c[1] == 'region' and x >= c[3] and y >= c[4] and x < c[3] + c[5] and y < c[4] + c[6] then
            last = c[7]
        end
    end
    return last
end
local function click(x, y)
    ui_mouse_at = {x, y}
    kernel.tick(0.016)         -- hover (and the regions under the cursor)
    local h = region_at(x, y)
    ui_events_queue = {{down = true, button = 0, x = x, y = y, region = h}, {down = false, button = 0, x = x, y = y, region = h}}
    kernel.tick(0.016)
end
local widgets
do
    local i = 1
    while true do
        local name, value = debug.getupvalue(kernel.dispatch, i)
        if not name then break end
        if name == 'handlers' then
            for _, h in ipairs(value['draw']) do
                local env = getfenv(h.fn)
                if env.widgets then widgets = env.widgets end
            end
        end
        i = i + 1
    end
end
click(15, 15)
ok(widgets and widgets.checked == true and widgets.changes == 1, 'checkbox toggles on a click')
click(15, 45)               -- opens the dropdown
click(20, 40 + 18 + 18 + 5) -- the list's second entry
ok(widgets.index == 2, 'dropdown picks the second entry (' .. tostring(widgets.index) .. ')')
click(15, 15)
ok(widgets.checked == false and widgets.changes == 2, 'checkbox toggles back')
ok(moved_windows[1] and moved_windows[1][1] == 512 and moved_windows[1][2] == 192 - 388, 'stored game window placed (' .. tostring(moved_windows[1] and moved_windows[1][1]) .. ')')
local listed = false
for _, p in ipairs(widgets.panels or {}) do
    if p.key == 'w:p' and p.title == 'Panel' and p.resizable and p.w == 200 then listed = true end
end
ok(listed, 'ui.panels lists the panels of the tick')
ok(widgets.ui.set_panel_rect('w:p', 300, 200, 100, 150), 'set_panel_rect on a drawn panel')
kernel.tick(0.016)
local pn = widgets.panel
ok(pn and math.abs(pn.x - 300) < 0.01 and math.abs(pn.y - 200) < 0.01 and math.abs(pn.w - 150) < 0.01 and math.abs(pn.h - 150) < 0.01,
   'the panel takes the place and size (min width kept): ' .. tostring(pn and pn.x) .. ',' .. tostring(pn and pn.w))
ok(not widgets.ui.set_panel_rect('w:nope', 0, 0), 'set_panel_rect on an unknown panel')
widgets.ui.reset_panel('w:p')
kernel.tick(0.016)
ok(widgets.panel.x == 400 and widgets.panel.w == 200, 'reset_panel: its own place again')
keys_held[0x25] = true
ok(widgets.ui.key_down('left') and not widgets.ui.key_down('right'), 'key_down')
keys_held[0x25] = nil
print(('%d passed, %d failed'):format(passes, failures))
if failures > 0 then
    for _, l in ipairs(logs) do print('  log: ' .. l) end
end
os.exit(failures == 0 and 0 or 1)
