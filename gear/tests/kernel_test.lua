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
    -- automation
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
        gear.task(function()
            record.task = 'started'
            gear.wait(1)
            record.task = 'waited'
            local got = gear.wait_until(function() return record.flag end, 5)
            record.task = got and 'flagged' or 'timeout'
        end)
        record.goblin = gear.data.object_types:find('goblin')
        record.search = #gear.data.object_types:search('o')
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
files.c = {
    ['mod.lua'] = "return {id = 'c', requires = {'missing'}}",
    ['main.lua'] = "error('must not run')",
}

local f = assert(io.open(kernel_path, 'rb'))
local source = f:read('*a')
f:close()
local chunk = assert(loadstring(source, '=kernel'))
local kernel = chunk(native)

ok(kernel.load() == 2, 'two mods load (c is skipped)')

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

print(('%d passed, %d failed'):format(passes, failures))
if failures > 0 then
    for _, l in ipairs(logs) do print('  log: ' .. l) end
end
os.exit(failures == 0 and 0 or 1)
