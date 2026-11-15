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
local ability_store = {}
local ability_active, damage_active, effect_active, bonus_active, slot_active
local clicks = 0
local level_active, constant_active
local ui_recording, ui_published = {}, {}
local pickup_published
save_block, save_calls, save_results = nil, {}, {}
ui_events_queue, keys_held, ui_mouse_at = {}, {}, {0, 0}
state_file, state_bak, state_writes = nil, nil, 0
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
        if name == 'ability_t1' then return 2, 0x78 end
        if name == 'ability_t2' then return 2, 0x94 end
    end,
    table_read = function(name, index, offset, kind)
        if name == 'object_types' and offset == 0x2E then return index == 2 and 3 or 8 end
        if name == 'ability_t1' or name == 'ability_t2' then
            if offset == 0 then return ({ability_t1 = {1000, 1001}, ability_t2 = {1, 2}})[name][index + 1] end
            local v = ability_store[name .. ':' .. index .. ':' .. offset]
            if v ~= nil then return v end
            return (name == 'ability_t1' and offset == 0x50) and 0.8 or 0
        end
        return 0
    end,
    table_write = function(name, index, offset, kind, value)
        local limits = {u8 = {0, 255}, i16 = {-32768, 32767}, u16 = {0, 65535}}
        local l = limits[kind]
        if l and (value < l[1] or value > l[2]) then return false end
        ability_store[name .. ':' .. index .. ':' .. offset] = value
        return true
    end,
    overlay_info = function(id) if id == 'ab' then return 2, 'C:/mods/ab/save' end end,
    overlay_resolve = function(path) return 'mapped:' .. path end,
    level_factor_active = function(v) level_active = v end,
    combat_constant_active = function(v) constant_active = v end,
    creature_ability_active = function(v) ability_active = v end,
    damage_active = function(v) damage_active = v end,
    effect_active = function(v) effect_active = v end,
    bonus_active = function(v) bonus_active = v end,
    skillslot_active = function(v) slot_active = v end,
    cursor_click = function() clicks = clicks + 1; return true end,
    difficulty = function() return 2 end,
    site_hooks = function() return 7, 5, 1 end,
    site_hook_sites = function() return {{hook = 'level.factor', index = 0, address = 5, calls = 2}} end,
    overlay_served = function(path) return path == 'bin/balance.bin' and 3 or 0 end,
    text_get = function(id) if id == 8193 then return 'Ability name' end end,
    setting = function(key) if key == 'SR_HD_WIDTH' then return '2560' end end,
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
    ui_item_icon = function(...) ui_recording[#ui_recording + 1] = {'item_icon', ...}; return true end,
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
    state_read = function() return state_file end,
    state_write = function(text, backup)
        if backup then state_bak = text else state_file = text; state_writes = state_writes + 1 end
        return true
    end,
    layout_read = function() return layout_file end,
    layout_write = function(text) layout_file = text; return true end,
    ui_block_keys = function(on) keys_blocked_by_native = on end,
    ui_can_move_windows = function() return true end,
    ui_move_window = function(slot, x, y, set) moved_windows[slot] = set and {x, y} or nil; return true end,
    save_blocked = function() return save_block end,
    save_request = function(slot, name, thumb)
        if save_block then return nil, save_block end
        save_calls[#save_calls + 1] = {slot, name, thumb}
        return true
    end,
    save_pending = function() return false end,
    save_info = function(slot) if slot <= 509 then return {exists = slot == 97, size = 5, mtime = 100} end end,
    save_poll = function() return table.remove(save_results, 1) end,
    builtin = function(name) if name == 'modmanager' then return "adm = admin; gear.on('frame', function() end)" end end,
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
        record.game = gear.game
        gear.on('game.saved', function(slot, info) record.saved = {slot, info.size} end)
        gear.on('game.save_failed', function(slot, message) record.save_failed = {slot, message} end)
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
files.ab = {
    ['mod.lua'] = "return {id = 'ab'}",
    ['main.lua'] = [[
        abil = {}
        local A = gear.data.abilities
        abil.t1 = A[1001]
        abil.t2 = A[2]
        abil.none = A[5555]
        abil.table1, abil.table2 = abil.t1.table, abil.t2.table
        abil.before = abil.t1.effect_base
        abil.name_text = abil.t1.name_text
        abil.ids = #A:ids()
        abil.fields = #A:fields(1001)
        -- a write through the record and through apply
        abil.t1.effect_base = 1.5
        abil.after = abil.t1.effect_base
        abil.n, abil.problems = A:apply({[1001] = {effect_per_level = 0.25, unk_40 = 7}, [2] = {value1_base = 12, target_mode = 3}})
        abil.applied = {A[1001].effect_per_level, A[1001].unk_40, A[2].value1_base, A[2].target_mode}
        -- nothing is written when one entry is wrong
        abil.bad_n, abil.bad_problems = A:apply({[1001] = {effect_base = 9}, [2] = {nope = 1}, [7777] = {x = 1}})
        abil.unchanged = A[1001].effect_base
        abil.bad_field = not pcall(function() A[1001].nope = 1 end)
        abil.bad_type = not pcall(function() A[1001].effect_base = 'x' end)
        abil.id_locked = not pcall(function() A[1001].id = 3 end)
        abil.range = not pcall(function() A[2].target_mode = 300 end)
        abil.set_view = not pcall(function() A[1] = {} end)
        abil.restored = A:restore()
        abil.after_restore = {A[1001].effect_base, A[1001].effect_per_level, A[1001].unk_40, A[2].value1_base, A[2].target_mode}
        abil.text = gear.text.get(8193)
        abil.no_text = gear.text.get(99)
        abil.overlay = gear.files.overlay()
        abil.resolved = gear.files.resolve('SRB/balance.bin')
        abil.served = {gear.files.served('bin/balance.bin'), gear.files.served('x')}
        abil.setting = gear.settings.get('SR_HD_WIDTH')
        abil.no_setting = gear.settings.get('NOPE')
        -- the level.factor site hook: tiers of 12 levels, and no answer above 40
        gear.on('creature.ability', function(t, vanilla, index) if t == 7 then return vanilla + 1 end end)
        gear.on('skillslot.select', function(index) if index == 2 then gear.game.cursor_click() end end)
        gear.on('bonus.constant', function(index, vanilla) if index == 2 then return vanilla + 1 end end)
        gear.on('effect.add', function(e) if e.ability == 7 then return {value = e.value, finish = true} end end)
        gear.on('damage', function(hit) if hit.victim_type == 3 then return hit.damage * 2 end end)
        gear.on('level.factor', function(level, site)
            if level > 40 then return nil end
            return 1 + (level > 12 and 1 or 0) + (level > 24 and 1 or 0) + (level > 36 and 1 or 0)
        end)
        abil.stats = gear.hooks.stats('level.factor')
        abil.sites = gear.hooks.sites()
        gear.on('combat.constant', function(name, vanilla, index)
            if name == 'hit.effect_scale' then return 1.0 end
            if name == 'difficulty.factor' then return vanilla * 0.5 end
            return nil
        end)
        abil.difficulty = gear.game.difficulty()
        abil.emit_constant = not pcall(gear.emit, 'combat.constant', 1)
        abil.emit_level = not pcall(gear.emit, 'level.factor', 1)
        -- writes that are not restored explicitly are undone when the mod is switched off
        A[1001].effect_base = 4
        gear.on('frame', function() error('off') end)
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

files.s = {
    ['mod.lua'] = [[return {
        id = 's', name = 'Settings mod', author = 'Me', description = 'Has settings', version = '2',
        config = {speed = 7},
        settings = {
            {id = 'flag', type = 'bool', label = 'Flag', default = true},
            {id = 'count', type = 'number', min = 0, max = 10, step = 2, default = 4, section = 'Numbers'},
            {id = 'mode', type = 'choice', choices = {'a', 'b', {value = 'c', label = 'See'}}, default = 'b'},
            {id = 'name', type = 'text', default = 'x', max_length = 5},
            {id = 'tint', type = 'color', default = 0xFF112233},
            {id = 'speed', type = 'number', min = 0, max = 100, default = 1},
            {id = 'limit', type = 'number', min = 0, max = 100, default = 1},
        },
        keys = {{id = 'open', label = 'Open', default = 'ctrl+k'}},
    }]],
    ['config.lua'] = 'return {limit = 3}',
    ['main.lua'] = [[
        srec = {changes = {}, gear = gear}
        gear.on('settings.changed', function(id, v) srec.changes[#srec.changes + 1] = id .. '=' .. tostring(v) end)
        gear.on('unload', function() srec.unloaded = (srec.unloaded or 0) + 1 end)
        gear.on('load', function() srec.loaded = true end)
        gear.on('frame', function() srec.frames = (srec.frames or 0) + 1 end)
        gear.every(0.1, function() srec.ticks = (srec.ticks or 0) + 1 end)
        gear.keys.on('open', function() srec.opened = (srec.opened or 0) + 1 end)
    ]],
}
files.t = {
    ['mod.lua'] = "return {id = 't', name = 'Dependent', requires = {'s'}, keys = {{id = 'open', label = 'Open', default = 'ctrl+k'}}}",
    ['main.lua'] = [[
        trec = {}
        gear.on('frame', function() trec.frames = (trec.frames or 0) + 1 end)
        gear.keys.on('open', function() trec.opened = (trec.opened or 0) + 1 end)
    ]],
}
files.m = {
    ['mod.lua'] = "return {id = 'm', settings = {{id = 'n', type = 'number', min = 0, max = 100, default = 1}}}",
    ['old.json'] = '{"n": 8}',
    ['main.lua'] = [[
        mrec = {gear = gear}
        mrec.migrated = gear.settings.migrate('old.json', function(t) gear.settings.set('n', t.n) end)
        gear.on('settings.changed', function() end)
    ]],
}
files.d = {['mod.lua'] = "return {id = 'd', enabled = false}", ['main.lua'] = 'drec = true'}
files.u = {['mod.lua'] = "return {id = 'u', settings = {{id = 'x', type = 'weird'}}}", ['main.lua'] = ''}

local f = assert(io.open(kernel_path, 'rb'))
local source = f:read('*a')
f:close()
local chunk = assert(loadstring(source, '=kernel'))
local kernel = chunk(native)

ok(kernel.load() == 8, 'eight mods load: a, b, w, s, t, m, ab and the manager (c, d and u are not)')

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


-- The ability tables, gear.text and gear.settings (mod ab). Its globals are reached through a handler's environment.
local abil
do
    local i = 1
    while true do
        local name, value = debug.getupvalue(kernel.dispatch, i)
        if not name then break end
        if name == 'handlers' then
            for _, h in ipairs(value['frame']) do
                local env = getfenv(h.fn)
                if env.abil then abil = env.abil end
            end
        end
        i = i + 1
    end
end
ok(abil ~= nil, 'found mod ab globals')
ok(abil.none == nil, 'an unknown ability id is nil')
ok(abil.table1 == 'T1' and abil.table2 == 'T2', 'abilities route to their table by id')
ok(abil.ids == 4, 'ids lists both tables')
ok(abil.fields > 20, 'fields lists the named and the unk_ fields')
ok(math.abs(abil.before - 0.8) < 1e-9 and abil.after == 1.5, 'a field reads and writes through the record')
ok(abil.n == 4 and abil.problems == nil, 'apply writes every field: ' .. tostring(abil.n))
ok(abil.applied[1] == 0.25 and abil.applied[2] == 7 and abil.applied[3] == 12 and abil.applied[4] == 3, 'apply wrote the values')
ok(abil.bad_n == nil and #abil.bad_problems == 2, 'apply reports every problem: ' .. tostring(abil.bad_problems and #abil.bad_problems))
ok(abil.unchanged == 1.5, 'apply writes nothing when an entry is wrong')
ok(abil.bad_field and abil.bad_type and abil.id_locked and abil.range and abil.set_view, 'bad writes are errors')
ok(abil.restored == 5, 'restore undoes every field this mod wrote: ' .. tostring(abil.restored))
ok(math.abs(abil.after_restore[1] - 0.8) < 1e-9 and abil.after_restore[2] == 0 and abil.after_restore[3] == 0
   and abil.after_restore[4] == 0 and abil.after_restore[5] == 0, 'the values are the original ones again')
ok(abil.text == 'Ability name' and abil.no_text == nil, 'gear.text.get')
ok(abil.overlay and abil.overlay.files == 2 and abil.overlay.saves == 'C:/mods/ab/save', 'gear.files.overlay')
ok(abil.resolved == 'mapped:SRB/balance.bin', 'gear.files.resolve')
ok(abil.served[1] == 3 and abil.served[2] == 0, 'gear.files.served')
ok(level_active == true, 'registering level.factor tells the host')
ok(kernel.level_factor(5, 0) == 1 and kernel.level_factor(13, 1) == 2 and kernel.level_factor(30, 2) == 3
   and kernel.level_factor(37, 3) == 4, 'level.factor answers with the handler')
ok(kernel.level_factor(50, 4) == nil, 'level.factor: no number, no answer')
ok(slot_active == true, 'registering skillslot.select tells the host')
kernel.skillslot_select(2); kernel.skillslot_select(1)
ok(clicks == 1, 'skillslot.select reaches the handler (and gear.game.cursor_click the host)')
ok(bonus_active == true, 'registering bonus.constant tells the host')
ok(kernel.bonus_constant(2, 5, 0) == 6 and kernel.bonus_constant(3, 5, 0) == nil, 'bonus.constant answers with the handler')
ok(effect_active == true, 'registering effect.add tells the host')
ok(kernel.effect_add({ability = 7, value = 2}).finish == true and kernel.effect_add({ability = 8}) == nil, 'effect.add answers with the handler table')
ok(damage_active == true, 'registering damage tells the host')
ok(kernel.damage(10, 3, 5, 0) == 20 and kernel.damage(10, 4, 5, 0) == nil, 'damage answers with the handler')
ok(ability_active == true, 'registering creature.ability tells the host')
ok(kernel.creature_ability(7, 25, 0, 0) == 26 and kernel.creature_ability(8, 25, 0, 0) == nil, 'creature.ability answers with the handler')
ok(abil.emit_level and abil.emit_constant, 'mods cannot emit level.factor or combat.constant')
ok(abil.difficulty == 2, 'gear.game.difficulty')
ok(constant_active == true, 'registering combat.constant tells the host')
ok(kernel.combat_constant('hit.effect_scale', 0.2, 2) == 1.0 and kernel.combat_constant('difficulty.factor', 2.0, 3) == 1.0
   and kernel.combat_constant('hit.chance_scale', 0.01, 0) == nil, 'combat.constant answers with the handler')
ok(abil.sites[1].hook == 'level.factor' and abil.sites[1].calls == 2, 'gear.hooks.sites')
ok(abil.stats.calls == 7 and abil.stats.answered == 5 and abil.stats.busy == 1, 'gear.hooks.stats')
ok(abil.setting == '2560' and abil.no_setting == nil, 'gear.settings.get')

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
-- mod ab raised errors in every frame and was switched off: the write it made last is undone
ok(math.abs(ability_store['ability_t1:1:80'] - 0.8) < 1e-9, 'switching a mod off undoes its ability writes')

-- gear.game: saving
do
    local game = rec.game
    ok(game and game.can_save() == true, 'can_save when nothing blocks')
    save_block = 'a window is open'
    local can, why = game.can_save()
    ok(can == false and why == 'a window is open', 'can_save gives the reason')
    local r, err = game.save(97, 'x')
    ok(r == nil and err == 'a window is open', 'save refused while blocked')
    save_block = nil
    ok(game.save(98, 'Autosave', {thumbnail = true}) == true and save_calls[1][1] == 98 and save_calls[1][2] == 'Autosave'
        and save_calls[1][3] == true, 'save passes slot, name and thumbnail')
    ok(game.save(5) == true and save_calls[2][2] == '' and save_calls[2][3] == false, 'save defaults')
    ok(game.save_info(97).exists and game.save_info(5000) == nil, 'save_info')
    save_results[1] = {slot = 98, ok = true, message = '', info = {exists = true, size = 77, mtime = 1}}
    save_results[2] = {slot = 99, ok = false, message = 'disk full', info = {exists = false, size = 0, mtime = 0}}
    kernel.tick(0.016)
    ok(rec.saved and rec.saved[1] == 98 and rec.saved[2] == 77, 'game.saved event')
    ok(rec.save_failed and rec.save_failed[1] == 99 and rec.save_failed[2] == 'disk full', 'game.save_failed event')
end

---------------------------------------------------------------------------------------------------------------
-- Mod manager: settings, saved state, key binds, switching mods on and off live.
---------------------------------------------------------------------------------------------------------------
local function handler_global(k, event, name)
    for i = 1, 200 do
        local up, value = debug.getupvalue(k.dispatch, i)
        if not up then return nil end
        if up == 'handlers' then
            for _, h in ipairs(value[event] or {}) do
                local env = getfenv(h.fn)
                if rawget(env, name) ~= nil then return env[name] end
            end
        end
    end
end
-- Another kernel (a restart): the kernel locks the string metatable, so unlock it for the new instance.
local function new_kernel()
    debug.getmetatable('').__metatable = nil
    return chunk(native)
end
local function entry_of(adm, id)
    for _, e in ipairs(adm.entries()) do if e.id == id then return e end end
end
kernel.flush()
local srec = handler_global(kernel, 'settings.changed', 'srec')
local adm = handler_global(kernel, 'frame', 'adm')
ok(srec ~= nil and adm ~= nil, 'found the settings mod and the manager stub')
local S = srec.gear.settings
ok(S.get('flag') == true and S.get('count') == 4 and S.get('mode') == 'b' and S.get('name') == 'x', 'setting defaults')
ok(S.get('speed') == 7, 'config overrides the default of a setting')
ok(S.get('tint') == 0xFF112233, 'color default')
ok(S.get('limit') == 3 and srec.gear.settings.all().limit == 3, 'config.lua overrides the default too')

-- set: coerced to the step, changed event only on change
ok(S.set('count', 5) == true and S.get('count') == 6, 'number is put on the step grid')
ok(srec.changes[#srec.changes] == 'count=6', 'settings.changed(id, value)')
local n = #srec.changes
S.set('count', 6)
ok(#srec.changes == n, 'no event when the value stays')
ok(S.set('count', 99) and S.get('count') == 10, 'number clamped to max')
ok(select(1, S.set('mode', 'zz')) == nil and S.get('mode') == 'b', 'unknown choice refused')
ok(S.set('mode', 'c') and S.get('mode') == 'c', 'choice')
ok(select(1, S.set('name', 'toolongtext')) == nil, 'text longer than max_length refused')
ok(S.set('tint', {255, 0, 0}) and S.get('tint') == 0xFFFF0000, 'color from {r,g,b}')
ok(S.set('tint', '#8000FF00') and S.get('tint') == 0x8000FF00, 'color from text')
ok(not pcall(S.get, 'nope'), 'unknown setting errors')
ok(not pcall(S.set, 'nope', 1), 'unknown setting errors on set')
S.reset('count')
ok(S.get('count') == 4 and srec.changes[#srec.changes] == 'count=4', 'reset(id) restores the default and tells the mod')
ok(adm.get('s', 'mode') == 'c' and adm.default('s', 'mode') == 'b', 'the manager reads value and default')

-- migration happens once
local mrec = handler_global(kernel, 'settings.changed', 'mrec')
ok(mrec and mrec.migrated == true and mrec.gear.settings.get('n') == 8, 'old settings file migrated on first start')

-- saved state
kernel.flush()
local saved = state_file
ok(saved and saved:find('"count"', 1, true) == nil, 'a reset setting is not saved')
ok(saved:find('"mode":"c"', 1, true) and saved:find('"migrated"', 1, true), 'settings and the migration flag are saved')
ok(adm.set_binding('s', 'open', 'Ctrl+J') == true and adm.binding('s', 'open') == 'ctrl+j', 'rebind (normalized)')
ok(select(1, adm.set_binding('s', 'open', 'foo+bar')) == nil, 'bad binding refused')
ok(adm.default_binding('s', 'open') == 'ctrl+k', 'default binding stays')

-- a new kernel reads the file back
do
    kernel.flush()
    local k2 = new_kernel()
    k2.load()
    local s2 = handler_global(k2, 'settings.changed', 'srec')
    ok(s2 and s2.gear.settings.get('mode') == 'c' and s2.gear.settings.get('speed') == 7, 'settings persist across a restart')
    ok(s2.gear.keys.binding('open') == 'ctrl+j', 'binding persists')
    -- the migrated file is not read again
    local m2 = handler_global(k2, 'settings.changed', 'mrec')
    ok(m2 and m2.migrated == false, 'migration does not run twice')
end

-- a broken file: kept as .bak, defaults apply, no crash
do
    local good = state_file
    state_file = '{"mods": {"s": {"settings": [1,'
    state_bak = nil
    local k3 = new_kernel()
    k3.load()
    local s3 = handler_global(k3, 'settings.changed', 'srec')
    ok(state_bak == '{"mods": {"s": {"settings": [1,', 'the unreadable file is kept as .bak')
    ok(s3 and s3.gear.settings.get('mode') == 'b', 'defaults after a broken file')
    -- wrong types inside a readable file are ignored value by value
    state_file = '{"mods":{"s":{"enabled":"yes","settings":{"count":"many","mode":"c","flag":null},"keys":{"open":"nonsense"}}}}'
    local k4 = new_kernel()
    k4.load()
    local s4 = handler_global(k4, 'settings.changed', 'srec')
    ok(s4 and s4.gear.settings.get('count') == 4 and s4.gear.settings.get('mode') == 'c' and s4.gear.settings.get('flag') == true,
       'invalid saved values fall back one by one')
    ok(s4.gear.keys.binding('open') == 'ctrl+k', 'invalid saved binding falls back')
    state_file = good
end

-- key binds
local function press(vks, down)
    for _, vk in ipairs(vks) do keys_held[vk] = down or nil end
end
local CTRL, SHIFT, J, K = 0x11, 0x10, 0x4A, 0x4B
local trec = handler_global(kernel, 'frame', 'trec')
ok(trec ~= nil, 'found mod t')
adm.set_binding('s', 'open', nil)          -- back to ctrl+k, which mod t binds too
local conflicts = adm.conflicts('s', 'open')
ok(#conflicts == 1 and conflicts[1].mod == 't', 'a key bound by two mods is a conflict')
ok(#adm.conflicts('gear-mods', 'open') == 0, 'no conflict for a unique key')
press({CTRL, K}, true)
kernel.tick(0.016)
ok(srec.opened == 1 and trec.opened == 1, 'both mods get a conflicting key')
kernel.tick(0.016)
ok(srec.opened == 1, 'a held key fires once')
press({K}, false); kernel.tick(0.016); press({K}, true); kernel.tick(0.016)
ok(srec.opened == 2, 'fires again after release')
press({SHIFT}, true); press({K}, false); kernel.tick(0.016); press({K}, true); kernel.tick(0.016)
ok(srec.opened == 2, 'extra modifier: no fire')
press({SHIFT, CTRL, K}, false)
adm.modal(true)
press({CTRL, K}, true); kernel.tick(0.016)
ok(srec.opened == 2, 'hotkeys stay quiet while the manager screen is open')
adm.modal(false)
press({CTRL, K}, false)
kernel.tick(0.016)
ok(adm.set_binding('s', 'open', 'ctrl+j') and #adm.conflicts('s', 'open') == 0, 'conflict gone after a rebind')
press({CTRL, J}, true); kernel.tick(0.016)
ok(srec.opened == 3 and trec.opened == 2, 'the new binding fires (and only that mod)')
press({CTRL, J}, false)
ok(adm.set_binding('s', 'open', '') and srec.gear.keys.binding('open') == '', 'unbound')

-- switching mods off and on, live
clock = clock + 1
kernel.tick(0.016)
local frames, ticks = srec.frames, srec.ticks
ok(frames and frames > 0 and ticks and ticks > 0, 'mod s runs frame handlers and timers')
ok(entry_of(adm, 's').state == 'on' and entry_of(adm, 't').state == 'on', 's and t are on')
ok(adm.set_enabled('s', false) == true, 'switch s off')
ok(entry_of(adm, 't').state == 'blocked' and entry_of(adm, 't').reason:find('requires s', 1, true), 'dependent t is blocked at once')
kernel.tick(0.016)
ok(srec.unloaded == 1, 'unload event reached the mod')
ok(entry_of(adm, 's').state == 'off' and entry_of(adm, 't').state == 'blocked' and not entry_of(adm, 't').loaded, 's and t are unloaded')
clock = clock + 5
kernel.tick(0.016); kernel.tick(0.016)
ok(srec.frames == frames and srec.ticks == ticks, 'no handler or timer of s runs any more')
ok(handler_global(kernel, 'settings.changed', 'srec') == nil, 'handlers are dropped')
press({CTRL, K}, true); kernel.tick(0.016); press({CTRL, K}, false)
ok(trec.opened == 2, 'key binds of an unloaded mod are gone')
kernel.flush()
ok(state_file:find('"enabled":false', 1, true), 'the off switch is saved')
-- settings of an off mod can still be read and changed
ok(adm.set('s', 'mode', 'a') and adm.get('s', 'mode') == 'a', 'settings of a mod that is off')
ok(adm.set_enabled('t', true) == true, 'switching t on switches s on too')
kernel.tick(0.016)
local srec2 = handler_global(kernel, 'settings.changed', 'srec')
ok(srec2 and srec2 ~= srec and srec2.loaded, 's loaded again, with a fresh environment, and got load')
ok(srec2.gear.settings.get('mode') == 'a', 'settings survive a reload')
ok(entry_of(adm, 's').state == 'on' and entry_of(adm, 't').state == 'on', 'both on again')
-- mod.lua enabled = false is only the default
ok(entry_of(adm, 'd').state == 'off', 'enabled = false in mod.lua: off by default')
ok(adm.set_enabled('d', true) and (kernel.tick(0.016) or true) and entry_of(adm, 'd').state == 'on', 'the player can switch it on')
-- a mod with errors: switched off, the toggle reloads it
ok(entry_of(adm, 'b').state == 'error' and entry_of(adm, 'b').errors >= 10, 'a mod switched off after errors shows as error')
adm.set_enabled('b', false); kernel.tick(0.016)
adm.set_enabled('b', true); kernel.tick(0.016)
ok(entry_of(adm, 'b').state == 'on' and entry_of(adm, 'b').errors < 10, 'switching it off and on starts it fresh')
-- missing requirement, invalid manifest
ok(entry_of(adm, 'c').state == 'off' or entry_of(adm, 'c').state == 'pending' or entry_of(adm, 'c').state == 'blocked', 'c is listed')
ok(select(1, adm.set_enabled('c', true)) == nil, 'cannot switch on a mod whose requirement is missing')
local invalid
for _, e in ipairs(adm.entries()) do if e.state == 'invalid' then invalid = e end end
ok(invalid and invalid.reason:find('type must be', 1, true), 'a bad manifest is listed with its message')
ok(select(1, adm.set_enabled('gear-mods', false)) == nil, 'the manager itself cannot be switched off')
-- public gear.mods
local list = srec2.gear.mods.list()
local found_t
for _, e in ipairs(list) do if e.id == 't' then found_t = e end end
ok(found_t and found_t.name == 'Dependent' and found_t.has_keys and not found_t.has_settings, 'gear.mods.list')
ok(srec2.gear.mods.get('s').author == 'Me' and srec2.gear.mods.get('nope') == nil, 'gear.mods.get')
-- custom page
srec2.gear.mods.page(function(x, y, w, h) srec2.page_args = {x, y, w, h}; return 40 end)
ok(adm.draw_page('s', 1, 2, 3, 4) == 40 and srec2.page_args[4] == 4, 'a mod page is drawn by the manager')
-- capture of a new binding
adm.capture_start()
ok(adm.capture() == nil, 'capture: nothing pressed')
press({CTRL, 0x4C}, true)
ok(adm.capture() == 'ctrl+l', 'capture builds the binding')
press({CTRL, 0x4C}, false)
-- item icons: an item table, a type id and a type name queue a native draw
do
    local ui2 = srec2.gear.ui
    ok(ui2.item_icon({id = 50, type = 2}, 4, 5, 64, 32) == true, 'item_icon with an item table')
    ok(ui2.item_icon(2, 0, 0, 32, 32, {layer = 'top'}) == true, 'item_icon with a type id')
    ok(ui2.item_icon('goblin', 0, 0, 32, 32) == true, 'item_icon with a type name')
    ok(not pcall(ui2.item_icon, 'no such type', 0, 0, 1, 1), 'item_icon with an unknown name errors')
    ok(not pcall(ui2.item_icon, nil, 0, 0, 1, 1), 'item_icon with nothing errors')
    kernel.tick(0.016)
    local icons = {}
    for _, c in ipairs(ui_published) do if c[1] == 'item_icon' then icons[#icons + 1] = c end end
    ok(#icons == 3 and icons[1][8] == 50 and icons[1][9] == 2 and icons[2][9] == 2 and icons[2][8] == 0 and icons[3][9] == 2,
       'the item icons reach the native side with item id and type (' .. #icons .. ')')
end
-- the game's own hotkeys count as conflicts
ok(adm.set_binding('s', 'open', 'ctrl+l') == true, 'bind a key the game uses')
do
    local c = adm.conflicts('s', 'open')
    ok(#c == 1 and c[1].game == true and c[1].label == 'Log book', "the game's own key is a conflict (" .. #c .. ")")
end
ok(adm.set_binding('s', 'open', 'f9') and adm.conflicts('s', 'open')[1].label == 'Quick save', "F9 is the game's quick save")
adm.set_binding('s', 'open', nil)
-- the keyboard is kept from the game while the screen is open (or a mod captures it)
ok(keys_blocked_by_native == false, 'no capture at rest')
adm.modal(true); kernel.tick(0.016)
ok(keys_blocked_by_native == true, 'the screen keeps the keyboard from the game')
adm.modal(false); kernel.tick(0.016)
ok(keys_blocked_by_native == false, 'and gives it back')
srec2.gear.ui.capture_keys(true); kernel.tick(0.016)
ok(keys_blocked_by_native == true, 'a mod can capture the keyboard')
adm.set_enabled('s', false); kernel.tick(0.016)
ok(keys_blocked_by_native == false, 'unloading the mod releases it')
adm.set_enabled('s', true); kernel.tick(0.016)


---------------------------------------------------------------------------------------------------------------
-- The real mod manager screen (gear/lua/modmanager.lua) driven through the mocked UI: open it with its hotkey,
-- page, open a mod's settings and keys, toggle, rebind, close.
---------------------------------------------------------------------------------------------------------------
do
    local rf = assert(io.open('gear/lua/modmanager.lua', 'rb'))
    local real = rf:read('*a')
    rf:close()
    local saved_builtin = native.builtin
    native.builtin = function(name) if name == 'modmanager' then return real end end
    state_file = nil
    local k = new_kernel()
    k.load()
    local log_start = #logs
    local function tick() k.tick(0.016) end
    local function texts()
        local out = {}
        for _, c in ipairs(ui_published) do
            if c[1] == 'text' then out[#out + 1] = c end
        end
        return out
    end
    local function find_texts(text)
        local out = {}
        for _, c in ipairs(texts()) do if c[4] == text then out[#out + 1] = c end end
        return out
    end
    local function has_text(text) return #find_texts(text) > 0 end
    local function has_prefix(prefix)
        for _, c in ipairs(texts()) do if c[4]:sub(1, #prefix) == prefix then return true end end
        return false
    end
    local function click_at(x, y)
        ui_mouse_at = {x, y}
        tick()
        local h
        for _, c in ipairs(ui_published) do
            if c[1] == 'region' and x >= c[3] and y >= c[4] and x < c[3] + c[5] and y < c[4] + c[6] then h = c[7] end
        end
        ui_events_queue = {{down = true, button = 0, x = x, y = y, region = h}, {down = false, button = 0, x = x, y = y, region = h}}
        tick()
        tick()
    end
    local function click_text(text, n)
        local c = find_texts(text)[n or 1]
        if not c then return false end
        click_at(c[5] + (c[7] > 0 and c[7] / 2 or 4), c[6] + (c[8] > 0 and c[8] / 2 or 6))
        return true
    end
    local function dump(label)
        if not os.getenv('GEAR_TEST_DUMP') then return end
        local t = {}
        for _, c in ipairs(texts()) do t[#t + 1] = c[4] end
        print(label .. ': ' .. table.concat(t, ' | '))
    end
    local function key(vk) keys_held[vk] = true; tick(); keys_held[vk] = nil; tick() end

    tick()
    key(0x7A)     -- F11
    ok(has_text('Mods') and has_text('Page 1 / 2'), 'F11 opens the screen with page 1 of 2')
    dump('page 1')
    ok(has_text('Dependent') and has_text('Mod manager') == false and has_text('10 installed'), 'rows of the first page')
    ok(has_prefix('Off: requires missing'), 'a mod whose requirement is missing says so')
    ok(has_prefix('Cannot be loaded') == false, 'invalid mods are on a later page')
    ok(click_text('>') and has_text('Page 2 / 2') and has_text('Mod manager'), 'next page shows the manager itself')
    dump('page 2')
    ok(has_prefix('Cannot be loaded'), 'an invalid manifest is listed with its message')
    ok(click_text('<') and has_text('Page 1 / 2'), 'previous page')
    click_text('>')
    ok(click_text('Settings', 2), 'click Settings of "Settings mod" (second on page 2)')
    dump('settings')
    ok(has_text('Reset to defaults') and has_text('Flag') and has_text('Numbers'), 'settings page: rows and section header')
    -- the bool checkbox: its text is 'On'
    local s_env = handler_global(k, 'settings.changed', 'srec')
    local before = s_env.gear.settings.get('flag')
    ok(click_text('On') and s_env.gear.settings.get('flag') == (not before), 'a checkbox changes the setting')
    ok(has_text('Reset'), 'a changed setting offers Reset')
    ok(click_text('Reset') and s_env.gear.settings.get('flag') == before, 'Reset restores the default')
    -- keys tab
    click_text('Keys', 1)
    dump('keys')
    ok(has_text('Ctrl+K') and has_text('Ctrl+K') and has_text('Open'), 'keys tab shows the binding')
    ok(has_prefix('Conflicts with: Dependent'), 'the conflict is marked')
    ok(click_text('Ctrl+K') and has_text('Press a key...'), 'click a binding to rebind')
    keys_held[0x11] = true; keys_held[0x55] = true
    tick()
    keys_held[0x11] = nil; keys_held[0x55] = nil
    tick()
    ok(s_env.gear.keys.binding('open') == 'ctrl+u' and has_text('Ctrl+U') and not has_prefix('Conflicts'), 'the new key is taken, no conflict')
    ok(has_text('Default'), 'a changed binding offers Default')
    ok(click_text('Default') and s_env.gear.keys.binding('open') == 'ctrl+k', 'Default restores it')
    -- back to the list, switch the mod off
    ok(click_text('< Back') and has_text('Page 2 / 2'), 'back to the list (same page)')
    ok(click_text('On', 2), 'switch "Settings mod" off')
    tick()
    ok(handler_global(k, 'settings.changed', 'srec') == nil and has_text('Off'), 'the mod is unloaded and its switch says Off')
    click_text('<')
    ok(has_prefix('Off: requires s'), 'the dependent shows why it is off')
    key(0x1B)    -- Escape closes the screen
    ok(has_text('Mods') == false, 'Escape closes the screen')
    for i = log_start + 1, #logs do
        if logs[i]:find('%[gear%-mods%] error') or logs[i]:find('error in key') then ok(false, 'manager error: ' .. logs[i]:sub(1, 300)) end
    end
    native.builtin = saved_builtin
end

print(('%d passed, %d failed'):format(passes, failures))
if failures > 0 then
    for _, l in ipairs(logs) do print('  log: ' .. l) end
end
os.exit(failures == 0 and 0 or 1)
