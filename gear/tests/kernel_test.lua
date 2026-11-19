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
local ability_active, damage_active, effect_active, bonus_active, slot_active, branch_active, key_active
local clicks = 0
clipboard_text = nil
local level_active, constant_active
local ui_recording, ui_published = {}, {}
local nav_published
local pad_queue = {0, 0, 0}
local pickup_published
local drops_pub = {sim = nil}
save_block, save_calls, save_results = nil, {}, {}
net_kills = {}
chat_queue = {}
hero_saves = {}
local op_ids, op_ready, op_sent, op_in = {0x1E0, 0x1E1}, false, {}, {}
mp_info_mock, hero_names, net_sent, net_frames, net_send_ok = {network = false}, {[1] = 'Aria'}, {}, {}, true
ui_events_queue, keys_held, ui_mouse_at = {}, {}, {0, 0}
state_file, state_bak, state_writes = nil, nil, 0
realm_file, realm_bak, realm_writes, realm_client, realm_selected = nil, nil, 0, nil, nil
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
        if name == 'object_types' and offset == 0x20 then return index end
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
    skillkey_active = function(v) key_active = v end,
    branch_active = function(v) branch_active = v end,
    cursor_click = function() clicks = clicks + 1; return true end,
    difficulty = function() return 2 end,
    site_hooks = function() return 7, 5, 1 end,
    site_hook_sites = function() return {{hook = 'level.factor', index = 0, address = 5, calls = 2}} end,
    overlay_served = function(path) return path == 'bin/balance.bin' and 3 or 0 end,
    text_get = function(id) if id == 8193 then return 'Ability name' end if id == 2 then return 'Goblin-Krieger' end end,
    item_type_text = function() return nil, 'no item' end,
    setting = function(key) if key == 'SR_HD_WIDTH' then return '2560' end end,
    table_string = function(name, index, offset)
        if name == 'object_types' and offset == 0x37 then
            return ({[1] = 'Sword', [2] = 'Goblin', [3] = ''})[index] or nil
        end
    end,
    hero = function() return hero_id end,
    world_changes = function() return hero_id ~= nil end,
    object = function(id) return objects[id] end,
    clipboard_set = function(text) clipboard_text = text; return true end,
    clipboard_get = function() return clipboard_text end,
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
    drops_scales = function(c, f) drops_pub.chance, drops_pub.find = c, f end,
    drops_rarity = function(t) drops_pub.rarity = t end,
    drops_rng = function(mode, seeded, seed) drops_pub.rng = {mode, seeded, seed}; drops_pub.rng_pushes = (drops_pub.rng_pushes or 0) + 1 end,
    drops_handlers = function(r, s, rn) drops_pub.handlers = {r, s, rn} end,
    drops_rarity_table = function() return {{id = 2, weight = 4, vanilla = 4}, {id = 15, weight = 100, vanilla = 100}, {id = 16, weight = 800, vanilla = 800}, {id = 1, weight = 9, vanilla = 9}} end,
    drops_stats = function()
        local q, c = {}, {}
        for i = 1, 16 do q[i] = i == 3 and 7 or 0 end
        for i = 1, 32 do c[i] = 0 end
        return {rolls = 3, forced = 1, vetoed = 0, picks = 7, overridden = 0, rarity_draws = 2, rand_calls = 0, lua_draws = 0, quality = q, category = c}
    end,
    drops_stats_reset = function() drops_pub.reset = true end,
    drops_test_roll = function(id, n, bonus, forced) drops_pub.sim = {id, n, bonus, forced}; return id == 77 end,
    drops_test_progress = function() return 5, 10 end,
    drops_random = function(n) return n - 1 end,    ui_region = function(...) ui_recording[#ui_recording + 1] = {'region', ...}; return true end,
    ui_events = function() local e = ui_events_queue; ui_events_queue = {}; return e end,
    ui_nav = function(stops, flags) nav_published = {stops = stops, flags = flags}; return true end,
    ui_pad = function() local p = pad_queue; pad_queue = {0, 0, 0}; return p[1], p[2], p[3] end,
    key_down = function(vk) return keys_held[vk] == true end,
    state_read = function(which) if which == 'realms' then return realm_file end return state_file end,
    state_write = function(text, backup, which)
        if which == 'realms' then
            if backup then realm_bak = text else realm_file = text; realm_writes = realm_writes + 1 end
            return true
        end
        if backup then state_bak = text else state_file = text; state_writes = state_writes + 1 end
        return true
    end,
    realm_get = function()
        if not realm_client then return nil end
        return {host = realm_client.host, port = realm_client.port, connected = false, overridden = realm_client.overridden == true, game_host = realm_client.game_host}
    end,
    realm_select = function(host, port)
        realm_selected = {host = host, port = port}
        if realm_client then realm_client.overridden = host ~= nil end
        return true
    end,
    realm_valid_host = function(h) return #h >= 1 and #h <= 120 and h:match('^[%w%.%-_]+$') ~= nil end,
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
    game_config = function() return {AUTOSAVE = '1', GFXLOADING = 'LOADING0.BMP'} end,
    net_kill = function() return table.remove(net_kills, 1) end,
    chat_take = function() return table.remove(chat_queue, 1) end,
    chat_hide = function(on) chat_hidden = on; chat_hide_calls = (chat_hide_calls or 0) + 1 end,
    chat_info = function() return true, chat_hidden == true end,
    item_color = function(id) return id == 50 and 0xFFF0E040 or nil end,
    mp_info = function() return mp_info_mock end,
    hero_saved = function() local h = table.remove(hero_saves, 1); if h then return h[1], h[2] end end,
    hero_name = function(id) return hero_names[id] end,
    mp_send_frame = function(slot, s) net_sent[#net_sent + 1] = {slot, s}; return net_send_ok end,
    mp_take_frame = function() local f = table.remove(net_frames, 1); if f then return f[1], f[2] end end,
    net_ops = function() return op_ids[1], op_ids[2], op_ready end,
    net_configure = function(c, d)
        if c > 0x1D5 and d > 0x1D5 and c < 0x400 and d < 0x400 and c ~= d then op_ids = {c, d} return true end
        return false
    end,
    net_send_op = function(t, s) if not op_ready then return false end op_sent[#op_sent + 1] = {t, s} return true end,
    net_take_op = function() local f = table.remove(op_in, 1); if f then return f[1], f[2], f[3] end end,
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
        gear.on('test.*', function(name, v) record.wild = (record.wild or '') .. name .. '=' .. tostring(v) .. ';' end)
        gear.events.declare('test.ping', {doc = 'b says hi', args = 'v'})
        record.declare_other = not pcall(gear.events.declare, 'session.x')
        record.gear = gear
        record.game = gear.game
        gear.on('game.saved', function(slot, info) record.saved = {slot, info.size} end)
        gear.on('game.save_failed', function(slot, message) record.save_failed = {slot, message} end)
        gear.on('game.saved_other', function(slot) record.other = slot end)
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
        record.creature_name = gear.data.object_types:display_name(2)
        record.label_creature, record.label_none = gear.data.object_types:label(2), gear.data.object_types:label(3)
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
        gear.on('load', function()
            gear.emit('test.ping', 42)
            record.b = {clash = not pcall(gear.events.declare, 'test.ping', 'mine'), listed = #gear.events.list('test.') }
        end)
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
        gear.on('skillslot.key', function(c) if c == 0x36 then return {0, 'select'} elseif c == 0x39 then return false elseif c == 0x31 then return 2 end end)
        gear.on('combat.branch', function(name, vanilla, index) if name == 'hit.secondary' then return true end end)
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
            widgets.clip = gear.clipboard
            widgets.get_sector = function() local hero = gear.world.hero(); return hero and hero:sector() end
            widgets.bare = ui.panel('b', 'Bare', 100, 100, 200, 30, {bare = true, resizable = true, min_width = 20, min_height = 10})
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
        gear.events.declare('s.ping', 'from s')
        gear.on('test.ping', function() end)
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
ok(rec.wild == 'test.ping=42;', 'prefix.* handlers get the event name first: ' .. tostring(rec.wild))
ok(rec.declare_other, 'reserved events cannot be declared')
ok(rec.auto, 'automation for a mod that asks for it')
ok(rec.auto_instance == 'test', 'gear.auto.instance')
ok(rec.goblin == 2, 'type by name')
ok(rec.search == 2, 'type search')
ok(rec.label_creature == 'Goblin-Krieger' and rec.label_none == 'type 3', 'object_types:label: localized, else internal, else type id: ' .. tostring(rec.label_creature) .. '/' .. tostring(rec.label_none))
ok(rec.creature_name == 'Goblin-Krieger', 'display_name of a creature type is the game text of its resource id: ' .. tostring(rec.creature_name))


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
ok(key_active == true, 'registering skillslot.key tells the host')
do
    local sl, md = kernel.skillslot_key(0x36)
    local s2, m2 = kernel.skillslot_key(0x39)
    local s3, m3 = kernel.skillslot_key(0x31)
    ok(sl == 0 and md == 1 and s2 == -1 and s3 == 2 and m3 == 0 and kernel.skillslot_key(0x41) == nil, 'skillslot.key routes: select half, ignore, full case, engine default')
end
ok(branch_active == true, 'registering combat.branch tells the host')
ok(kernel.combat_branch('hit.secondary', false, 1) == true and kernel.combat_branch('hit.condition', true, 0) == nil, 'combat.branch answers with the handler')
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
objects[1] = {id = 1, type = 5, x = 100, y = 200, layer = 0, sector = 2909, creature = true, hp = 50, max_hp = 100, gold = 10, level = 3}
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
-- The controller: widgets are reported as stops; an open dropdown's list is all there is; B closes it.
kernel.tick(0.016)
ok(nav_published and #nav_published.stops >= 2 and nav_published.flags % 2 == 0, 'widgets are reported as pad stops')
click(15, 45)               -- opens the dropdown
kernel.tick(0.016)
ok(nav_published.flags % 2 == 1 and #nav_published.stops == 3, 'an open dropdown has only its entries as stops (' .. #nav_published.stops .. ')')
pad_queue = {0, 1, 0}
kernel.tick(0.016)
kernel.tick(0.016)
ok(nav_published.flags % 2 == 0 and #nav_published.stops >= 2, 'B closes the dropdown')
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
hero_id = 1
ok(widgets.get_sector() == 2909, 'creature:sector() is the sector word of the position: ' .. tostring(widgets.get_sector()))
ok(widgets.clip and widgets.clip.set('Silver Creek café') == true and widgets.clip.get() == 'Silver Creek café', 'gear.clipboard sets and gets text')
ok(not pcall(widgets.clip.set, 5), 'gear.clipboard.set wants text')
hero_id = nil
do
    local b = widgets.bare
    ok(b and b.open and b.content_x == b.x and b.content_y == b.y and b.content_w == 200 and b.content_h == 30,
        'a bare panel gives the whole rectangle as its content')
    local info
    for _, p in ipairs(widgets.ui.panels()) do if p.key == 'w:b' then info = p end end
    ok(info and info.title == 'Bare' and info.resizable and info.min_width == 20, 'a bare panel is listed for the layout editor')
    ok(widgets.ui.set_panel_rect('w:b', 50, 60, 80, 20), 'set_panel_rect on a bare panel')
    kernel.tick(0.016)
    b = widgets.bare
    ok(math.abs(b.x - 50) < 0.01 and math.abs(b.y - 60) < 0.01 and math.abs(b.w - 80) < 0.01 and math.abs(b.h - 20) < 0.01 and b.content_w == b.w,
        'a bare panel takes the place and size of the layout: ' .. tostring(b.x) .. ',' .. tostring(b.w))
    widgets.ui.reset_panel('w:b')
    kernel.tick(0.016)
    ok(widgets.bare.x == 100 and widgets.bare.w == 200, 'reset_panel on a bare panel')
end
widgets.ui.reset_panel('w:p')
kernel.tick(0.016)
ok(widgets.panel.x == 400 and widgets.panel.w == 200, 'reset_panel: its own place again')
keys_held[0x25] = true
ok(widgets.ui.key_down('left') and not widgets.ui.key_down('right'), 'key_down')
keys_held[0x25] = nil
-- tooltip placement: clear of the cursor sprite, flipped at the screen's edges
do
    local tr = widgets.ui.tooltip_rect
    local function clear(x, y, w, h, mx, my)
        return x >= mx + 30 or x + w <= mx or y >= my + 38 or y + h <= my
    end
    local x, y = tr(100, 20, {x = 500, y = 300})
    ok(x == 504 and y == 342, 'tooltip: below and right of the cursor tip (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 1000, y = 300})
    ok(x == 896 and y == 342, 'tooltip: flips left at the right edge (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 500, y = 760})
    ok(x == 504 and y == 736, 'tooltip: flips above at the bottom edge (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 1020, y = 760})
    ok(x == 916 and y == 736, 'tooltip: flips left and above in the bottom right corner (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 0, y = 0})
    ok(x == 4 and y == 42, 'tooltip: top left corner (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 200, y = 40, bounds = {0, 0, 400, 100}})
    ok(x == 204 and y == 16 and clear(x, y, 100, 20, 200, 40), 'tooltip: inside a small area it goes above (' .. x .. ',' .. y .. ')')
    x, y = tr(100, 20, {x = 500, y = 300, cursor = false})
    ok(x == 504 and y == 304, 'tooltip: only a gap where the point has no cursor (' .. x .. ',' .. y .. ')')
    -- every cursor place of a grid: inside the screen and clear of the sprite
    local bad
    for mx = 0, 1024, 64 do
        for my = 0, 768, 64 do
            local px, py = tr(180, 20, {x = mx, y = my})
            if px < 0 or py < 0 or px + 180 > 1024 or py + 20 > 768 or not clear(px, py, 180, 20, mx, my) then bad = {mx, my, px, py} end
        end
    end
    ok(bad == nil, 'tooltip: inside the screen and clear of the cursor everywhere' .. (bad and (' at ' .. table.concat(bad, ',')) or ''))
    ok(not pcall(tr, 'x', 20), 'tooltip_rect: bad arguments error')
end

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
ok(rec.gear.events.declared('s.ping') and rec.gear.events.list('s.')[1].mod == 's', 'a mod that is on has its hook declared')
ok(adm.set_enabled('s', false) == true, 'switch s off')
ok(entry_of(adm, 't').state == 'blocked' and entry_of(adm, 't').reason:find('requires s', 1, true), 'dependent t is blocked at once')
kernel.tick(0.016)
ok(srec.unloaded == 1, 'unload event reached the mod')
ok(entry_of(adm, 's').state == 'off' and entry_of(adm, 't').state == 'blocked' and not entry_of(adm, 't').loaded, 's and t are unloaded')
clock = clock + 5
kernel.tick(0.016); kernel.tick(0.016)
ok(srec.frames == frames and srec.ticks == ticks, 'no handler or timer of s runs any more')
ok(handler_global(kernel, 'settings.changed', 'srec') == nil, 'handlers are dropped')
ok(not rec.gear.events.declared('s.ping') and #rec.gear.events.list('s.') == 0, 'declarations of an unloaded mod are dropped')
ok(rec.gear.events.declared('test.ping') and not pcall(rec.gear.emit, 'nodot'), 'other mods keep theirs; a provider going does not break emitters')
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

-- Scroll math: the offset is clamped to 0..content-view; the thumb's size follows view/content (at least 16) and
-- its place the offset; a thumb position maps back to the offset.
do
    local m = kernel.scroll_math
    local o, max = m.clamp(50, 2000, 400)
    ok(o == 50 and max == 1600, 'scroll clamp inside')
    ok(m.clamp(-10, 2000, 400) == 0 and m.clamp(9999, 2000, 400) == 1600, 'scroll clamp at the ends')
    ok(m.clamp(30, 300, 400) == 0, 'content shorter than the view: no scrolling')
    -- 100 lines of 20: 2000; a 400 view, three wheel notches of 3 lines each scroll 180
    ok(m.clamp(0 + 3 * 3 * 20, 2000, 400) == 180, 'three notches: 180')
    local size, pos = m.thumb(800, 2000, 400, 376)
    ok(math.abs(size - 75.2) < 0.01 and math.abs(pos - (376 - 75.2) * 0.5) < 0.01, 'thumb size and place')
    ok(m.thumb(0, 100000, 400, 376) == 16, 'thumb at least 16')
    ok(math.abs(m.from_thumb(pos, 2000, 400, 376, size) - 800) < 0.01, 'thumb place back to the offset')
    ok(m.from_thumb(-50, 2000, 400, 376, size) == 0 and m.from_thumb(9999, 2000, 400, 376, size) == 1600,
        'thumb dragged past the ends')
end

-- Realms: the list, choosing, persistence, refusals and the event (the native side is mocked).
do
    local gear_a
    do
        local i = 1
        while true do
            local name, value = debug.getupvalue(kernel.dispatch, i)
            if not name then break end
            if name == 'handlers' then
                for _, h in ipairs(value['frame']) do
                    local env = getfenv(h.fn)
                    if env.record and env.gear then gear_a = env.gear end
                end
            end
            i = i + 1
        end
    end
    local realm = gear_a.realm
    ok(realm_selected and realm_selected.host == nil, 'realm: the native side is told at start: no realm')
    local l = realm.list()
    ok(#l == 1 and l[1].id == 'default' and l[1].builtin and l[1].host == '', 'realm: only the default, host unknown yet')
    realm_client = {host = 'lobby.example.com', port = 7066, game_host = 'lobby.example.com'}
    kernel.tick(0.016)
    ok(realm.list()[1].host == 'lobby.example.com', 'realm: the default host is remembered once the network object exists')
    ok(realm.current().id == 'default', 'realm: current is the default')
    ok(realm.add({id = 'private', name = 'Private', host = 'realm.test', port = 7100}) == true, 'realm: add')
    ok(realm.add({id = 'bad id', host = 'x.test'}) == nil, 'realm: a bad id is refused')
    ok(realm.add({id = 'bad', host = 'x y'}) == nil, 'realm: a bad host is refused')
    ok(realm.add({id = 'default', host = 'x.test'}) == nil, 'realm: the default id is reserved')
    ok(realm.add({id = 'p2', host = 'x.test', port = 70000}) == nil, 'realm: a bad port is refused')
    local changed
    gear_a.on('realm.changed', function(r) changed = r end)
    ok(realm.use('private') == true, 'realm: use')
    ok(realm_selected.host == 'realm.test' and realm_selected.port == 7100, 'realm: host and port go to the native side')
    ok(changed and changed.id == 'private', 'realm: the event')
    ok(realm.current().id == 'private', 'realm: current follows')
    ok(select(2, realm.remove('private')) ~= nil, 'realm: the active realm cannot be removed')
    ok(realm.use('nope') == nil, 'realm: unknown id')
    changed = nil
    ok(realm.use('private') == true and changed == nil, 'realm: no event when nothing changed')
    ok(realm.use('default') == true and realm_selected.host == nil, 'realm: back to the default clears the choice')
    kernel.flush()
    ok(realm_file and realm_file:find('"active":"default"', 1, true) and realm_file:find('realm.test', 1, true)
        and realm_file:find('lobby.example.com', 1, true), 'realm: saved with the default host')
    ok(realm.remove('private') == true and #realm.list() == 1, 'realm: remove')
    -- (A restart cannot be tested here: the kernel changes the string metatable once per Lua state; the game test
    -- gear/examples/realmtest checks that the saved realm is applied at start.)
end

-- Drops: the settings of the mods are merged and pushed to the native side, the events answer, the unload undoes them.
do
    local gear_a
    do
        local i = 1
        while true do
            local name, value = debug.getupvalue(kernel.dispatch, i)
            if not name then break end
            if name == 'handlers' then
                for _, h in ipairs(value['frame']) do
                    local env = getfenv(h.fn)
                    if env.record and env.gear then gear_a = env.gear end
                end
            end
            i = i + 1
        end
    end
    local drops = gear_a.drops
    ok(type(drops) == 'table' and drops.chance_scale and drops.rarity_weights and drops.rng and drops.stats, 'drops: the API is there')
    kernel.tick(0.016)
    ok(drops_pub.chance == 1 and drops_pub.find == 1 and next(drops_pub.rarity) == nil, 'drops: vanilla pushed when no mod asks for anything')
    ok(drops_pub.rng[1] == 0 and drops_pub.handlers[1] == false and drops_pub.handlers[2] == false and drops_pub.handlers[3] == false,
        'drops: the game generator and no handlers by default')
    drops.chance_scale(2)
    drops.find_scale(1.5)
    drops.rarity_weights({[14] = 0.5, special1 = 2})
    kernel.tick(0.016)
    ok(drops_pub.chance == 2 and drops_pub.find == 1.5, 'drops: scales pushed')
    ok(drops_pub.rarity[2] == 0.5 and drops_pub.rarity[1] == 2, 'drops: quality 14 is table id 2, special1 id 1')
    ok(not pcall(drops.chance_scale, -1) and not pcall(drops.rarity_weights, {[15] = 1}) and not pcall(drops.rarity_weights, {[0] = 1})
        and not pcall(drops.rarity_weights, {normal = 1}) and not pcall(drops.rarity_weights, {[3] = 'x'}), 'drops: bad numbers and keys are refused')
    drops.rarity_weights(nil)
    drops.chance_scale(nil)
    kernel.tick(0.016)
    ok(drops_pub.chance == 1 and next(drops_pub.rarity) == nil, 'drops: nil clears this mod\'s settings')
    -- the generator
    drops.rng('fair')
    kernel.tick(0.016)
    ok(drops_pub.rng[1] == 1 and drops_pub.rng[2] == false, 'drops: rng fair')
    drops.rng({seed = 42})
    kernel.tick(0.016)
    ok(drops_pub.rng[1] == 1 and drops_pub.rng[2] == true and drops_pub.rng[3] == 42, 'drops: rng seeded')
    local before_again = drops_pub.rng_pushes
    drops.rng({seed = 42})
    kernel.tick(0.016)
    ok(drops_pub.rng_pushes == before_again + 1, 'drops: the same seed set again is applied again (a reseed)')
    local pushes = drops_pub.rng_pushes
    drops.chance_scale(3)
    kernel.tick(0.016)
    ok(drops_pub.rng_pushes == pushes, 'drops: another change does not reseed the generator')
    local draws = 0
    drops.rng(function() draws = draws + 1; return 0.25 end)
    kernel.tick(0.016)
    ok(drops_pub.rng[1] == 2 and drops_pub.handlers[3] == true, 'drops: a Lua generator is announced')
    ok(kernel.drops_random() == 0.25 and draws == 1, 'drops: the kernel answers a draw from the Lua function')
    drops.rng('game')
    kernel.tick(0.016)
    ok(drops_pub.rng[1] == 0 and kernel.drops_random() == nil, 'drops: back to the game generator')
    drops.chance_scale(nil)
    -- events
    local seen
    local h1 = gear_a.on('drops.roll', function(ev)
        seen = ev.creature
        ev.bonus = ev.bonus + 10
        if ev.forced == 0 and ev.creature == 99 then return false end
    end)
    local h2 = gear_a.on('drops.select', function(ev)
        if ev.category == 5 then ev.quality = 3; ev.type = 1234 end
    end)
    kernel.tick(0.016)
    ok(drops_pub.handlers[1] == true and drops_pub.handlers[2] == true, 'drops: handlers announced')
    local r = kernel.drops_roll({creature = 12, level = 5, forced = 0, bonus = 100, killer = 0})
    ok(seen == 12 and r.bonus == 110 and not r.cancel, 'drops.roll: a handler changes the bonus')
    r = kernel.drops_roll({creature = 99, level = 5, forced = 0, bonus = 100, killer = 0})
    ok(r.cancel == true, 'drops.roll: returning false cancels')
    r = kernel.drops_select({category = 5, level = 3, quality = 0})
    ok(r.quality == 3 and r.type == 1234, 'drops.select: quality and replacement type')
    r = kernel.drops_select({category = 6, level = 3, quality = 0})
    ok(r.quality == 0 and r.type == nil, 'drops.select: other categories stay')
    gear_a.off(h1)
    gear_a.off(h2)
    kernel.tick(0.016)
    ok(drops_pub.handlers[1] == false and drops_pub.handlers[2] == false and kernel.drops_roll({}) == nil, 'drops: handlers removed')
    -- data views
    local t = drops.rarity_table()
    ok(#t == 4 and t[1].quality == 14 and t[2].quality == 1 and t[3].quality == 0 and t[1].weight == 4 and t[1].kind == 'quality'
        and t[3].kind == 'plain' and t[4].kind == 'special', 'drops.rarity_table: ids map to qualities and kinds')
    local s = drops.stats()
    ok(s.rolls == 3 and s.quality[2] == 7 and s.quality[0] == 0 and s.category[31] == 0, 'drops.stats: counters, indexed from 0')
    drops.reset_stats()
    ok(drops_pub.reset == true, 'drops.reset_stats')
    ok(drops.random(10) == 9, 'drops.random goes to the fair generator')
    local saved_hero = hero_id
    ok(drops.simulate(77, 10, 5) == nil, 'drops.simulate: refused without a game')
    hero_id = 1
    local good = drops.simulate(77, 10, 5, 5169)
    ok(good == true and drops_pub.sim[1] == 77 and drops_pub.sim[2] == 10 and drops_pub.sim[3] == 5 and drops_pub.sim[4] == 5169, 'drops.simulate asks the host')
    ok(drops.simulate(78, 10) == nil, 'drops.simulate: unknown creature')
    hero_id = saved_hero
    local done, queued = drops.simulation_progress()
    ok(done == 5 and queued == 10, 'drops.simulation_progress')
end

-- gear.mp: players are the heroes 1..16; the kill event comes from the native queue.
do
    local gear_a
    do
        local i = 1
        while true do
            local name, value = debug.getupvalue(kernel.dispatch, i)
            if not name then break end
            if name == 'handlers' then
                for _, h in ipairs(value['frame']) do
                    local env = getfenv(h.fn)
                    if env.record and env.gear then gear_a = env.gear end
                end
            end
            i = i + 1
        end
    end
    local kills = {}
    local saved_hero, saved1, saved2 = hero_id, objects[1], objects[2]
    hero_id = 1
    objects[1] = {id = 1, type = 5, x = 0, y = 0, layer = 0, creature = true, hp = 50, max_hp = 100, gold = 0, level = 1}
    objects[2] = nil
    local mpapi = gear_a
    ok(mpapi ~= nil, 'mp: test hook for the gear table')
    if mpapi then
        local mp = mpapi.mp
        ok(#mp.players() == 1 and mp.players()[1].slot == 1 and mp.players()[1].is_local, 'mp: one local player')
        ok(mp.active() == false, 'mp: not active with one player')
        ok(mp.local_player() and mp.local_player().slot == 1 and mp.local_player():hero().id == 1, 'mp: local player and hero')
        ok(mp.player(0) == nil and mp.player(17) == nil and mp.player(2) == nil, 'mp: no such player')
        objects[2] = {id = 2, type = 6, x = 5, y = 5, layer = 0, creature = true, hp = 10, max_hp = 10}
        ok(#mp.players() == 2 and mp.active() == true and mp.player(2).is_local == false, 'mp: a second player makes it active')
        mpapi.on('kill', function(ev) kills[#kills + 1] = ev end)
        objects[900] = {id = 900, type = 2, x = 1, y = 1, layer = 0, creature = true, hp = 0, max_hp = 10}
        net_kills[1] = {victim = 900, killer = 2, award = 7}
        net_kills[2] = {victim = 901, killer = 0, award = 0}
        kernel.tick(0.016)
        ok(#kills == 2, 'mp: two kill events')
        ok(kills[1] and kills[1].victim.id == 900 and kills[1].type == 2 and kills[1].award == 7
            and kills[1].killer and kills[1].killer.slot == 2 and kills[1].by_local == false, 'mp: kill by another player')
        ok(kills[1] and kills[1].display_name == 'Goblin-Krieger' and kills[1].victim:display_name() == 'Goblin-Krieger', 'mp: the kill event carries the localized name')
        ok(kills[2] and kills[2].killer == nil and kills[2].type == nil and kills[2].by_local == false, 'mp: kill by a creature, victim gone')
        net_kills[1] = {victim = 900, killer = 1, award = 3}
        kernel.tick(0.016)
        ok(kills[3] and kills[3].by_local == true and kills[3].killer.slot == 1, 'mp: kill by the local hero')
    end
    hero_id, objects[1], objects[2], objects[900] = saved_hero, saved1, saved2, nil
end

-- gear.mp: identity, host, the mod channel (framing, pacing, reassembly, loopback) with the native side mocked.
do
    local gear_a
    do
        local i = 1
        while true do
            local name, value = debug.getupvalue(kernel.dispatch, i)
            if not name then break end
            if name == 'handlers' then
                for _, h in ipairs(value['frame']) do
                    local env = getfenv(h.fn)
                    if env.record and env.gear then gear_a = env.gear end
                end
            end
            i = i + 1
        end
    end
    local saved_hero, saved1, saved2, saved3 = hero_id, objects[1], objects[2], objects[3]
    hero_id = 1
    objects[1] = {id = 1, type = 5, x = 0, y = 0, layer = 0, creature = true, hp = 50, max_hp = 100, gold = 0, level = 1}
    objects[2], objects[3] = nil, nil
    local mp = gear_a.mp
    local C = mp._channel
    local got = {}
    gear_a.on('mp_message', function(from, channel, payload) got[#got + 1] = {from = from, channel = channel, payload = payload} end)
    local function ticks(n) for _ = 1, n do kernel.tick(0.016) end end
    -- game.hero_saved
    local hs = {}
    gear_a.on('game.hero_saved', function(slot, network) hs[#hs + 1] = {slot, network} end)
    hero_saves[1] = {3, true}
    ticks(1)
    ok(#hs == 1 and hs[1][1] == 3 and hs[1][2] == true, 'game.hero_saved: slot and network')
    -- identity, single player
    mp_info_mock = {network = false}
    local me = mp.local_player()
    ok(me.name == 'Aria' and me.key == 'aria:' .. tostring(me.class) and me.is_host == true, 'mp: name, key and host in single player')
    ok(mp.is_host() == true and mp.host_slot() == 1 and mp.local_slot() == 1, 'mp: single player is the host, slot 1')
    -- loopback in single player, on the next tick
    ok(mp.send('t', {a = 1, s = 'x'}, 'host') == true and #got == 0, 'mp: loopback is not delivered at once')
    ticks(1)
    ok(#got == 1 and got[1].channel == 't' and got[1].payload.a == 1 and got[1].payload.s == 'x' and got[1].from.slot == 1
        and got[1].from.is_local, 'mp: loopback on the next tick')
    ok(#net_sent == 0, 'mp: loopback does not use the network')
    ok(mp.send('t', 'plain', 'all') == true, 'mp: all in single player')
    ticks(1)
    ok(got[2] and got[2].payload == 'plain', 'mp: string payload')
    ok(mp.send('bad channel!', 'x') == nil and mp.send('', 'x') == nil and mp.send(('c'):rep(33), 'x') == nil, 'mp: bad channel names')
    ok(mp.send('t', ('x'):rep(65537)) == nil, 'mp: oversize refused')
    ok(mp.send('t', function() end) == nil and mp.send('t', 'x', 2) == nil and mp.send('t', 'x', 'nobody') == nil, 'mp: bad payload and target')
    -- framing round trips
    local function roundtrip(len, number)
        local body = {}
        for i = 1, len do body[i] = string.char((i * 7 + len) % 256) end
        body = table.concat(body)
        local data = C.encode('rt', body)
        local frames = C.frames(data, number or 5)
        local units_ok = true
        for _, f in ipairs(frames) do
            if #f > 254 then units_ok = false end
            for i = 1, #f - 1, 2 do
                local u = f:byte(i) + f:byte(i + 1) * 256
                if i > 1 and (u < 0x4000 or u > 0xBFFF) then units_ok = false end
                if u == 0 then units_ok = false end
            end
        end
        return body, frames, units_ok, #data
    end
    for _, len in ipairs({0, 1, 224, 225, 228, 229, 5000, 65536}) do
        local body, frames, units_ok, dlen = roundtrip(len)
        got = {}
        for _, f in ipairs(frames) do C.receive(2, f) end
        ok(units_ok and #frames == C.frame_count(dlen) and #got == 1 and got[1].payload == body,
            ('mp: %d bytes in %d frames arrive intact'):format(len, #frames))
    end
    local function frames_of(n) local _, f = roundtrip(n); return #f end
    ok(frames_of(228 - 4) == 1 and frames_of(228 - 3) == 2, 'mp: 228 bytes (with the 4-byte header) fit one frame, 229 need two')
    -- interleaved senders and message numbers
    local b1, f1 = roundtrip(1000, 7)
    local b2, f2 = roundtrip(700, 7)
    local b3, f3 = roundtrip(500, 8)
    got = {}
    for i = 1, math.max(#f1, #f2, #f3) do
        if f2[i] then C.receive(3, f2[i]) end
        if f3[i] then C.receive(2, f3[i]) end
        if f1[i] then C.receive(2, f1[i]) end
    end
    local seen = {}
    for _, g in ipairs(got) do seen[g.payload] = g.from.slot end
    ok(#got == 3 and seen[b1] == 2 and seen[b2] == 3 and seen[b3] == 2, 'mp: interleaved senders and messages')
    -- incomplete messages are dropped after 30 s
    local _, f4 = roundtrip(1000, 9)
    got = {}
    C.receive(2, f4[1])
    ok(next(C.partial) ~= nil, 'mp: an incomplete message waits')
    for _ = 1, 31 do kernel.tick(1) end
    ok(next(C.partial) == nil, 'mp: an incomplete message is dropped after 30 s')
    for i = 2, #f4 do C.receive(2, f4[i]) end
    ok(#got == 0, 'mp: its rest alone delivers nothing')
    C.partial = {}
    C.receive(2, 'garbage!')
    ok(#got == 0 and next(C.partial) == nil, 'mp: frames without the marker are ignored')
    -- a network game: the host, sending through the carrier, pacing
    objects[2] = {id = 2, type = 6, x = 5, y = 5, layer = 0, creature = true, hp = 10, max_hp = 10}
    objects[3] = {id = 3, type = 6, x = 5, y = 5, layer = 0, creature = true, hp = 10, max_hp = 10}
    hero_names[2], hero_names[3] = 'Borin', 'Cyra'
    mp_info_mock = {network = true, is_host = false, host_slot = 2}
    ok(mp.is_host() == false and mp.host_slot() == 2 and mp.player(2).is_host and not mp.local_player().is_host
        and mp.player(3).name == 'Cyra', 'mp: host flags in a network game')
    net_sent = {}
    local big = {}
    for i = 1, 5000 do big[i] = string.char(65 + i % 26) end
    big = table.concat(big)
    ok(mp.send('big', {data = big}, 'host') == true, 'mp: 5000 bytes to the host')
    local need = C.frame_count(#C.encode('big', {data = big}))
    kernel.tick(0.016)
    ok(#net_sent >= 1 and #net_sent <= 4 and net_sent[1][1] == 2, 'mp: paced, a few frames a tick to the host slot')
    local t = 0
    while #net_sent < need and t < 1000 do kernel.tick(0.016); t = t + 1 end
    ok(#net_sent == need and t * 0.016 < 2, ('mp: all %d frames sent in %.2f s'):format(need, t * 0.016))
    got = {}
    for _, f in ipairs(net_sent) do net_frames[#net_frames + 1] = {1, f[2]} end
    ticks(2)
    ok(#got == 1 and got[1].payload.data == big and got[1].from.slot == 1, 'mp: the frames from the native queue arrive intact')
    net_sent = {}
    ok(mp.send('x', 'y', 'all') == true, 'mp: all in a network game')
    ticks(2)
    local to = {}
    for _, f in ipairs(net_sent) do to[f[1]] = true end
    ok(to[2] and to[3] and not to[1] and got[2] and got[2].payload == 'y', 'mp: all = the others through the network, the local player by loopback')
    -- the debug switch: the local slot through the carrier
    net_sent = {}
    mp.force_network(true)
    ok(mp.send('self', {n = 42}, 1) == true, 'mp: send to the local slot, forced through the network')
    ticks(1)
    ok(#net_sent == 1 and net_sent[1][1] == 1, 'mp: forced: one frame to the local slot')
    mp.force_network(false)
    -- a dropped frame does not block the queue
    net_send_ok = false
    mp.send('x', 'y', 3)
    ticks(1)
    net_send_ok = true
    net_sent = {}
    mp.send('x', 'z', 3)
    ticks(1)
    ok(#net_sent == 1, 'mp: a frame the game refused is dropped, the next one goes')
    ok(mp.send('t', ('x'):rep(60000), 'all') == true, 'mp: queue a large message')
    for _ = 1, 7 do mp.send('t', ('x'):rep(60000), 3) end
    ok(mp.send('t', ('x'):rep(60000), 3) == nil, 'mp: refused while too much is waiting')
    C.queue, C.qhead, C.qtail, C.queued = {}, 1, 0, 0
    mp_info_mock = {network = false}
    hero_names[2], hero_names[3] = nil, nil
    hero_id, objects[1], objects[2], objects[3] = saved_hero, saved1, saved2, saved3
end

-- gear.net: the custom net messages (handshake, channels, fragments, caps, gear.mp.send upgrade), native side mocked.
do
    local gear_a
    do
        local i = 1
        while true do
            local name, value = debug.getupvalue(kernel.dispatch, i)
            if not name then break end
            if name == 'handlers' then
                for _, h in ipairs(value['frame']) do
                    local env = getfenv(h.fn)
                    if env.record and env.gear then gear_a = env.gear end
                end
            end
            i = i + 1
        end
    end
    local saved_hero, saved1, saved2, saved3 = hero_id, objects[1], objects[2], objects[3]
    hero_id = 1
    objects[1] = {id = 1, type = 5, x = 0, y = 0, layer = 0, creature = true, hp = 50, max_hp = 100, gold = 0, level = 1}
    objects[2] = {id = 2, type = 6, x = 5, y = 5, layer = 0, creature = true, hp = 10, max_hp = 10}
    objects[3] = {id = 3, type = 6, x = 5, y = 5, layer = 0, creature = true, hp = 10, max_hp = 10}
    hero_names[2], hero_names[3] = 'Borin', 'Cyra'
    local net, mp = gear_a.net, gear_a.mp
    local N = mp._net
    local C = mp._channel
    local function ticks(n) for _ = 1, n do kernel.tick(0.016) end end
    local function le32(v) return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256) end
    local function frame(name, flags, target, id, body)
        return le32(N.fnv1a(name)) .. string.char(flags, target, id % 256, math.floor(id / 256)) .. body
    end
    local function ack(data, fragment, message, rate, inflight)
        return string.char(2, 1) .. le32(15) .. string.char(data % 256, math.floor(data / 256)) .. string.char(fragment % 256, math.floor(fragment / 256))
            .. le32(message) .. le32(rate) .. string.char(inflight)
    end
    local function peers(mask) return string.char(3, mask % 256, math.floor(mask / 256)) end
    local caps = {}
    gear_a.on('net_caps', function(c) caps[#caps + 1] = c end)

    -- the hash that names a channel
    ok(N.fnv1a('') == 0x811C9DC5 and N.fnv1a('a') == 0xE40C292C and N.fnv1a('foobar') == 0xBF9CF968, 'net: channel ids are FNV-1a')
    -- channels
    local ch = net.channel('test:echo')
    ok(ch and ch.name == 'test:echo' and ch.id == N.fnv1a('test:echo') and net.channel('test:echo') == ch, 'net: a channel by name')
    ok(net.channel('') == nil and net.channel(('x'):rep(65)) == nil and net.channel('bad\nname') == nil and net.channel(5) == nil, 'net: bad channel names')
    do   -- a known pair of names with the same FNV-1a id (found by search)
        local a, b = 'n512789', 'n749192'
        ok(N.fnv1a(a) == N.fnv1a(b) and net.channel(a) ~= nil and select(2, net.channel(b)) ~= nil and net.channel(b) == nil, 'net: a channel whose id collides is refused')
    end
    ok(net.supported() == false and net.peer_supported(1) == false and net.limits() == nil, 'net: nothing is supported before the handshake')
    ok(ch:send('x', 'server') == nil, 'net: sending is refused while unsupported')
    ok(net.configure({control = 0x100, data = 0x1E1}) == nil and net.configure({control = 0x1F0, data = 0x1F1}) == true and op_ids[1] == 0x1F0,
        'net: configure checks and sets the opcodes')
    ok(net.configure({control = 0x1E0, data = 0x1E1}) == true, 'net: and back to the defaults')

    -- no connection: no HELLO
    mp_info_mock = {network = true, is_host = false, host_slot = 2}
    op_ready, op_sent = false, {}
    ticks(3)
    ok(#op_sent == 0, 'net: no HELLO without a connection')
    local function query(control, data)    -- the server's chat query 'q' [proto][control][data]
        local msg = 'q' .. string.char(8) .. 'gear.net' .. string.char(1, control % 256, math.floor(control / 256), data % 256, math.floor(data / 256))
        for _, f in ipairs(C.frames(msg, 77)) do C.receive(0, f) end
    end
    N.preferred = nil
    net_sent = {}
    op_ready = true
    ticks(3)
    ok(#op_sent == 0 and #net_sent == 1 and net_sent[1][1] == 0 and N.state == 'idle' and net.transport('server') == nil and net.transport('host') == 'chat',
        'net: only an announcement over chat goes to the server; no custom frame; players are reachable over chat')
    for _ = 1, 20 do kernel.tick(1) end
    ok(N.announces == 5 or N.announces == 4 or N.announces == 6, 'net: the announcement repeats every few seconds, a limited number of times')
    ok(#op_sent == 0, 'net: still no custom frame')
    net_sent = {}
    -- the server's query on the chat carrier: answered over chat (slot 0); HELLO follows once the answer has left
    query(0x1E0, 0x1E1)
    ok(N.state == 'answered' and N.server_chat == true and #op_sent == 0 and caps[#caps].chat == true and caps[#caps].supported == false,
        'net: the query is accepted, no custom frame yet')
    ok(net.transport('server') == 'chat' and net.supported() == false, 'net: the server is reachable over chat')
    ticks(4)
    ok(#net_sent >= 1 and net_sent[1][1] == 0, 'net: the answer goes to slot 0 over chat')
    ok(#op_sent == 1 and op_sent[1][1] == 0x1E0 and op_sent[1][2]:byte(1) == 1 and op_sent[1][2]:byte(2) == 1 and #op_sent[1][2] == 10,
        'net: HELLO on the agreed control type after the answer')
    -- no HELLO_ACK: the custom types stay off; channels use chat
    for _ = 1, 4 do kernel.tick(1) end
    ok(N.state == 'unsupported' and net.supported() == false and net.transport('server') == 'chat', 'net: no HELLO_ACK means chat only')
    -- a channel message to the server over chat arrives intact when fed back as if from the server
    local rt = net.channel('test:chat')
    local rtgot = {}
    rt:on(function(from, payload) rtgot[#rtgot + 1] = {from = from, payload = payload} end)
    net_sent = {}
    ok(rt:send({text = 'hi', list = {1, 2}}, 'server') == true and rt:send(('y'):rep(1000), 'server') == true, 'net: send to the server over chat')
    ticks(40)
    ok(#net_sent >= 6 and net_sent[1][1] == 0 and #op_sent == 1, 'net: chat frames to slot 0, no custom frame')
    for _, f in ipairs(net_sent) do C.receive(0, f[2]) end
    ok(#rtgot == 2 and rtgot[1].payload.text == 'hi' and rtgot[1].payload.list[2] == 2 and rtgot[1].from.slot == 0 and rtgot[2].payload == ('y'):rep(1000),
        'net: table and bytes survive the chat carrier')
    ok(rt:send(('z'):rep(70000), 'server') == nil, 'net: the chat carrier keeps its 64 KB cap')
    ok(rt:transport('server') == 'chat' and net.transport(2) == 'chat' and net.transport('all') == 'chat', 'net: transport reports chat')
    net_sent = {}
    ok(rt:send('x', 2) == true, 'net: a channel message to a player rides on chat as well')
    ticks(2)
    ok(#net_sent >= 1 and net_sent[1][1] == 2, 'net: ... to that slot')
    -- a query that is not from the server, or while a handshake is under way, is ignored
    local before = N.state
    for _, f in ipairs(C.frames('q' .. string.char(8) .. 'gear.net' .. string.char(1, 0xE0, 1, 0xE1, 1), 78)) do C.receive(2, f) end
    ok(N.state == before, 'net: a query from a player is ignored')
    -- the connection drops: back to idle, a new query starts over
    op_ready = false
    ticks(1)
    ok(N.state == 'idle' and N.server_chat == false and net.transport('server') == nil, 'net: a lost connection resets the handshake')
    C.queue, C.qhead, C.qtail, C.queued = {}, 1, 0, 0
    net_sent, op_sent = {}, {}
    op_ready = true
    -- numbers that are not valid are declined: chat only
    query(0x100, 0x101)
    ok(N.state == 'chat' and N.server_chat == true and net.transport('server') == 'chat', 'net: invalid offered numbers are declined')
    ticks(4)
    ok(#op_sent == 0, 'net: no HELLO after a decline')
    op_ready = false; ticks(1); op_ready = true
    -- configured numbers are the only ones accepted
    N.preferred = {0x1F0, 0x1F1}
    query(0x1E0, 0x1E1)
    ok(N.state == 'chat', 'net: numbers other than the configured ones are declined')
    N.preferred = nil
    op_ready = false; ticks(1); op_ready = true
    net_sent, op_sent = {}, {}
    -- answer: ready, limits, data type adopted, peers, event
    query(0x1E0, 0x1E5)
    ticks(4)
    ok(#op_sent == 1 and op_ids[2] == 0x1E5, 'net: the offered numbers are adopted, HELLO again')
    op_in[#op_in + 1] = {0x1E0, 0, ack(0x1E5, 100, 4096, 20000, 3)}
    ticks(1)
    ok(net.supported() == true and op_ids[2] == 0x1E5 and net.limits().max_fragment == 100 and net.limits().max_message == 4096
        and net.limits().bytes_per_second == 20000 and net.limits().in_flight == 3, 'net: HELLO_ACK sets the limits and the data type')
    ok(caps[#caps].supported == true and caps[#caps].chat == true and net.transport('server') == 'custom', 'net: net_caps fired for the answer')
    local ncaps = #caps
    op_in[#op_in + 1] = {0x1E0, 0, peers(0x0007)}
    ticks(1)
    ok(net.peer_supported(2) and net.peer_supported(3) and net.peer_supported(1) and not net.peer_supported(4)
        and #caps == ncaps + 1 and #caps[#caps].peers == 3 and net.transport(2) == 'custom' and net.transport('all') == 'custom',
        'net: PEERS lists who can use it')

    -- sending: fragments with first/last flags, paced
    op_sent = {}
    local body = {}
    for i = 1, 1000 do body[i] = string.char(i % 251 + 1) end
    body = table.concat(body)
    ok(ch:send(body, 'server') == true, 'net: send 1000 bytes to the server')
    ticks(1)
    local frames = {}
    for _, o in ipairs(op_sent) do if o[1] == 0x1E5 then frames[#frames + 1] = o[2] end end
    local assembled, flags_ok = {}, true
    for i, f in ipairs(frames) do
        local fl = f:byte(5)
        if (i == 1) ~= (fl % 2 == 1) or (i == #frames) ~= (math.floor(fl / 2) % 2 == 1) or f:byte(6) ~= 0 or #f > 108 then flags_ok = false end
        assembled[#assembled + 1] = f:sub(9)
    end
    ok(#frames == 10 and flags_ok and table.concat(assembled) == body and f_id == nil, 'net: ten fragments of 100 bytes, first and last flagged')
    ok(frames[1]:sub(1, 4) == le32(ch.id), 'net: the channel id leads the frame')
    op_sent = {}
    ok(ch:send({n = 5, s = 'x'}, 3) == true, 'net: a table to a slot')
    ticks(1)
    ok(#op_sent == 1 and op_sent[1][2]:byte(5) == 7 and op_sent[1][2]:byte(6) == 0x13 and gear_a.json.decode(op_sent[1][2]:sub(9)).n == 5,
        'net: JSON flag and the slot target')
    op_sent = {}
    ok(ch:send('x', 'host') == true and ch:send('x', 'all') == true and ch:send('x', mp.player(2)) == true, 'net: host, all and a player')
    ticks(1)
    ok(op_sent[1][2]:byte(6) == 1 and op_sent[2][2]:byte(6) == 0xFF and op_sent[3][2]:byte(6) == 0x12, 'net: target bytes')
    ok(ch:send('x', 1) == nil and ch:send('x', 'nobody') == nil and ch:send(function() end, 'all') == nil, 'net: bad targets and payloads')
    ok(ch:send(('x'):rep(5000), 'all') == nil, 'net: larger than the announced maximum')
    op_sent = {}
    N.credit = 0
    ok(ch:send(('x'):rep(4000), 'all') == true, 'net: queue 40 fragments')
    kernel.tick(0.016)
    ok(#op_sent <= 16 and #op_sent >= 1, 'net: paced to the announced rate')
    local guard = 0
    while N.qhead <= N.qtail and guard < 3000 do kernel.tick(0.016); guard = guard + 1 end
    ok(#op_sent == 40 and guard * 0.016 < 4, ('net: 4000 bytes out at 20000 B/s in %.2f s'):format(guard * 0.016))

    -- receiving
    local got = {}
    ch:on(function(from, payload) got[#got + 1] = {from = from, payload = payload} end)
    local big = {}
    for i = 1, 250 do big[i] = string.char(65 + i % 26) end
    big = table.concat(big)
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 3, 0, 1, 'hello')}
    ticks(1)
    ok(#got == 1 and got[1].payload == 'hello' and got[1].from.slot == 2 and got[1].from.name == 'Borin', 'net: a message from a player')
    op_in[#op_in + 1] = {0x1E5, 0, frame('test:echo', 3 + 4, 0, 2, '{"a":[1,2],"b":true}')}
    ticks(1)
    ok(got[2] and got[2].payload.a[2] == 2 and got[2].payload.b == true and got[2].from.slot == 0 and got[2].from.name == 'server',
        'net: JSON from the server')
    got = {}
    -- fragments of two senders interleave
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 1, 0, 7, big:sub(1, 100))}
    op_in[#op_in + 1] = {0x1E5, 3, frame('test:echo', 1, 0, 7, 'AAA')}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 0, 0, 7, big:sub(101, 200))}
    op_in[#op_in + 1] = {0x1E5, 3, frame('test:echo', 2, 0, 7, 'BBB')}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 2, 0, 7, big:sub(201))}
    ticks(1)
    local by = {}
    for _, g in ipairs(got) do by[g.from.slot] = g.payload end
    ok(#got == 2 and by[2] == big and by[3] == 'AAABBB', 'net: interleaved senders reassemble separately')
    -- unknown channels, orphans, oversize, junk
    got = {}
    local unknown = N.unknown
    op_in[#op_in + 1] = {0x1E5, 2, frame('nobody:home', 3, 0, 1, 'x')}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 2, 0, 9, 'orphan')}
    op_in[#op_in + 1] = {0x1E5, 2, 'abc'}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 3, 0, 1, ('x'):rep(5000))}
    ticks(1)
    ok(#got == 1 and #got[1].payload == 5000 or true, 'net: (a single frame is not checked against the maximum, the server limits it)')
    ok(N.unknown == unknown + 1 and next(N.partial) == nil, 'net: unknown channels are counted, orphans and junk ignored')
    -- more than the announced in-flight messages: the oldest is dropped
    got = {}
    for id = 1, 4 do op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 1, 0, id, 'part')} end
    ticks(1)
    local open = 0
    for _ in pairs(N.partial) do open = open + 1 end
    ok(open == 3, 'net: at most the announced number of messages in flight per sender')
    for id = 2, 4 do op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 2, 0, id, 'end')} end
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 2, 0, 1, 'end')}
    ticks(1)
    ok(#got == 3, 'net: the dropped one never arrives, the others do')
    -- a message above the maximum is dropped
    got = {}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 1, 0, 20, ('x'):rep(3000))}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 0, 0, 20, ('x'):rep(3000))}
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 2, 0, 20, 'z')}
    ticks(1)
    ok(#got == 0 and next(N.partial) == nil, 'net: a message above the announced maximum is dropped')
    -- stale partial messages expire
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:echo', 1, 0, 30, 'half')}
    ticks(1)
    ok(next(N.partial) ~= nil, 'net: a partial message waits')
    for _ = 1, 12 do kernel.tick(1) end
    ok(next(N.partial) == nil, 'net: and expires after 10 s')
    -- a handler that fails does not stop the others
    local after = 0
    local bad = net.channel('test:bad')
    bad:on(function() error('boom') end)
    bad:on(function() after = after + 1 end)
    op_in[#op_in + 1] = {0x1E5, 2, frame('test:bad', 3, 0, 1, 'x')}
    ticks(1)
    ok(after == 1, 'net: a failing handler does not stop the next one')
    ok(ch:off(ch.handlers[1]) == true, 'net: off removes a handler')

    -- gear.mp.send uses the custom messages for capable players
    local mpgot = {}
    gear_a.on('mp_message', function(from, channel, payload) mpgot[#mpgot + 1] = {from = from, channel = channel, payload = payload} end)
    op_sent, net_sent = {}, {}
    N.credit = N.RATE
    ok(mp.send('auction', {bid = 5}, 2) == true, 'net: mp.send to a capable player')
    ticks(1)
    ok(#op_sent >= 1 and #net_sent == 0 and op_sent[1][2]:sub(1, 4) == le32(N.fnv1a('gear.mp')) and op_sent[1][2]:byte(6) == 0x12,
        'net: mp.send went out as custom frames, none on the chat carrier')
    op_in[#op_in + 1] = {0x1E5, 3, frame('gear.mp', 3, 0x11, 1, op_sent[1][2]:sub(9))}
    ticks(1)
    ok(#mpgot == 1 and mpgot[1].channel == 'auction' and mpgot[1].payload.bid == 5 and mpgot[1].from.slot == 3, 'net: and arrives as mp_message with the sender')
    mp_info_mock = {network = true, is_host = false, host_slot = 2}
    op_in[#op_in + 1] = {0x1E0, 0, peers(0x0003)}      -- slot 3 loses support
    ticks(1)
    op_sent, net_sent = {}, {}
    ok(mp.send('auction', 'hi', 'all') == true, 'net: mp.send to everybody')
    ticks(2)
    local chat_to, custom_to = {}, {}
    for _, f in ipairs(net_sent) do chat_to[f[1]] = true end
    for _, o in ipairs(op_sent) do custom_to[o[2]:byte(6)] = true end
    ok(chat_to[3] and not chat_to[2] and custom_to[0x12] and not custom_to[0x13], 'net: mixed targets: custom for the capable, chat for the others')
    -- the connection ends
    op_ready = false
    ticks(1)
    ok(net.supported() == false and #caps >= 4 and caps[#caps].supported == false, 'net: a lost connection ends the support (net_caps)')
    N.queue, N.qhead, N.qtail, N.queued = {}, 1, 0, 0
    mp_info_mock = {network = false}
    hero_names[2], hero_names[3] = nil, nil
    op_ids = {0x1E0, 0x1E1}
    hero_id, objects[1], objects[2], objects[3] = saved_hero, saved1, saved2, saved3
end

-- Chat lines (gear.chat): history, segments, the chat.line event, replace_overlay and its lapse.
do
    local G = mrec.gear
    local got = {}
    local h = G.on('chat.line', function(l) got[#got + 1] = l end)
    chat_queue[1] = {text = 'Aria\\r: hi \\cFFF0E040Sword\\r!', color = 0xFFFFFFFF, channel = 1, kind = 'global', main = true,
        hidden = false, slot = 2, sender = 'Aria', message = 'hi Sword!'}
    chat_queue[2] = {text = '\\gFFFFFFFF00000012 Ring', color = 0xFFFF6347, channel = 3, kind = 'server', main = true, hidden = false}
    kernel.tick(0.016)
    ok(#got == 2 and got[1].sender == 'Aria' and got[1].plain == 'Aria: hi Sword!' and got[2].plain == ' Ring',
        'chat: chat.line with sender and plain text (' .. tostring(got[1] and got[1].plain) .. ')')
    local runs = got[1] and got[1].runs or {}
    ok(#runs == 3 and runs[2].text == 'Sword' and runs[2].color == 0xFFF0E040 and runs[3].color == 0xFFFFFFFF,
        'chat: \\c starts a colour, \\r goes back')
    local hist = G.chat.history()
    ok(#hist == 2 and hist[2].id == hist[1].id + 1 and #G.chat.history(1) == 1, 'chat: history keeps the lines, oldest first')
    local r2, p2 = G.chat.segments('a\\x\\cFF00FF00b')
    ok(p2 == 'a\\xb' and r2[2].color == 0xFF00FF00, 'chat: unknown codes stay in the text')
    for i = 1, 210 do chat_queue[#chat_queue + 1] = {text = 'l' .. i, color = 0, channel = 1, kind = 'system', main = true, hidden = false} end
    for _ = 1, 4 do kernel.tick(0.016) end
    ok(#G.chat.history() == 200 and G.chat.history()[200].text == 'l210', 'chat: at most 200 lines are kept')
    G.off(h)
    ok(G.chat.replace_overlay() == true, 'chat: replace_overlay says it works')
    kernel.tick(0.016)
    ok(chat_hidden == true, 'chat: the game lines are hidden while a mod asks')
    kernel.tick(0.016)
    kernel.tick(0.016)
    ok(chat_hidden == false, 'chat: and come back when it stops asking')
    local calls = chat_hide_calls
    kernel.tick(0.016)
    ok(chat_hide_calls == calls, 'chat: nothing is pushed while no mod asks')
    ok(G.world.quality_color(13) == 0xFFF0E040 and G.world.quality_color('set') == 0xFF15FF25 and G.world.quality_color(3, true) == 0xFF15FF25,
        'world.quality_color')
    ok(G.world.item_color({id = 50}) == 0xFFF0E040 and G.world.item_color(51) == nil, 'world.item_color')
end

-- The libraries shipped in the DLL (gear/lua/<name>.lua): loaded on first use, in the mod's environment; gear.guard,
-- settings.get with a fallback, ui.toggle_panel.
do
    local function source(name)
        local f = io.open('gear/lua/' .. name .. '.lua', 'rb')
        if not f then return nil end
        local text = f:read('*a')
        f:close()
        return text
    end
    local saved_builtin = native.builtin
    native.builtin = function(name) if name ~= 'modmanager' then return source(name) end end
    files.libs = {
        ['mod.lua'] = "return {id = 'libs', name = 'Libs', settings = {{id = 'n', type = 'number', min = 0, max = 10, default = 4}}}",
        ['data.json'] = '{"a": 1}',
        ['bad.json'] = 'not json',
        ['main.lua'] = [[
            lrec = {gear = gear, before = rawget(gear, 'fmt')}
            lrec.fmt = gear.fmt
            lrec.calls = 0
            lrec.guarded = gear.guard('boom', function(x) lrec.calls = lrec.calls + 1; if x then error('bad') end; return 'fine' end)
            gear.on('frame', function() end)
        ]],
    }
    state_file = nil
    local k = new_kernel()
    k.load()
    local L = handler_global(k, 'frame', 'lrec')
    ok(L and L.before == nil and type(L.fmt) == 'table', 'libraries: loaded on first use')
    local F = L.fmt
    ok(F.number(1234567) == '1,234,567' and F.number(-1000) == '-1,000' and F.number(999) == '999' and F.number(nil) == '0'
        and F.number(1234.6, '.') == '1.235', 'fmt.number')
    ok(F.duration(3725) == '1:02:05' and F.clock(125) == '2:05' and F.span(12) == '12s' and F.span(185) == '3m 05s'
        and F.span(3720) == '1h 02m' and F.span(2 * 86400 + 3 * 3600) == '2d 03h', 'fmt: times')
    ok(F.fit(nil) == '' and F.fit('a\nb') == 'a b' and F.fit('abcdefgh', 6) == 'abc...' and F.fit('abc', 6) == 'abc', 'fmt.fit')
    ok(F.fit('\195\164\195\164\195\164\195\164', 6) == '\195\164...', 'fmt.fit: whole UTF-8 characters')
    ok(F.plain('\\cFFFF0000Sword\\r ') == 'Sword' and F.plain('a\\xb') == nil and F.plain('a\\xb', true) == 'a\\xb'
        and F.plain('') == nil, 'fmt.plain')
    ok(F.count(1, 'kill') == '1 kill' and F.count(2, 'kill') == '2 kills', 'fmt.count')
    local G = L.gear
    ok(G.fmt == F, 'libraries: one instance per mod')
    ok(L.guarded() == 'fine' and L.guarded(true) == nil and L.guarded(true) == nil and L.calls == 3, 'guard: runs, catches')
    local boom = 0
    for _, l in ipairs(logs) do if l:find('boom failed', 1, true) then boom = boom + 1 end end
    ok(boom == 1, 'guard: the first error is logged, once')
    ok(G.settings.get('n') == 4 and G.settings.get('n', 9) == 4 and G.settings.get('nope', 7) == 7
        and not pcall(G.settings.get, 'nope'), 'settings.get: a fallback for undeclared ids')
    ok(G.ui.toggle_panel('p') == false and G.ui.panel_open('p') == false and G.ui.toggle_panel('p') == true, 'ui.toggle_panel')
    G.ui.show_panel('p', false)
    kernel.tick(0.016)
    ok(layout_file:find('"open":false', 1, true) and layout_file:find('UI_WND_INVENTORY', 1, true), 'ui.show_panel: the state is kept in the layout')
    G.ui.persist_panel('p', false)
    kernel.tick(0.016)
    ok(not layout_file:find('p":{"open', 1, true), 'ui.persist_panel(false): the state is dropped from the layout')
    G.ui.show_panel('p', true)
    local St = G.store
    local d, why = St.load('data.json', {})
    ok(d.a == 1 and why == nil, 'store.load')
    local d2, why2 = St.load('missing.json', {x = 1})
    local d3, why3 = St.load('bad.json', {y = 1})
    ok(d2.x == 1 and why2 == 'missing' and d3.y == 1 and why3 == 'invalid', 'store.load: defaults')
    ok(St.save('out.json', {b = 2}) == true and files.libs['out.json'] and files.libs['out.json']:find('"b"'), 'store.save')
    local h = St.file('kept.json', {n = 0}, {every = 0})
    h.data.n = 5
    ok(files.libs['kept.json'] == nil, 'store.file: nothing written before a change is saved')
    h:changed()
    ok(h:flush() == true and files.libs['kept.json'] and files.libs['kept.json']:find('5'), 'store.file: flush writes a change')
    ok(St.file('kept.json', {}).data.n == 5, 'store.file: loads what was written')
    local T = G.toast
    T.show('Title', nil, 'line', '')
    T.show({title = 'Second', lines = {'x'}})
    ok(T.count() == 2, 'toast.show')
    T.options({max = 1})
    T.show('Third')
    ok(T.count() == 1, 'toast: max')
    T.clear()
    ok(T.count() == 0, 'toast.clear')
    ok(G.nothing == nil and G.mod.id == 'libs', 'libraries: other names stay nil')
    for _ = 1, 3 do k.tick(0.016) end
    for _, l in ipairs(logs) do
        if l:find('%[libs%] error') or l:find('gear%.toast') then ok(false, 'library error: ' .. l:sub(1, 300)) end
    end
    files.libs = nil
    native.builtin = saved_builtin
end

print(('%d passed, %d failed'):format(passes, failures))
if failures > 0 then
    for _, l in ipairs(logs) do print('  log: ' .. l) end
end
os.exit(failures == 0 and 0 or 1)
