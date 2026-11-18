-- Tests of the client mods sessiontracker, lootlog and damagemeter (gear/examples) outside the game: the `gear`
-- global is faked (events, a hero, items, a recording UI) and each mod's main.lua runs against it. Run with LuaJIT
-- from the repository root:
--     luajit gear/tests/session_mods_test.lua
-- Exit code 0: all passed. This checks the mods' logic and that they survive nil / missing data; it does not draw
-- anything and says nothing about the real engine (see docs/GearApi.md "Session mods").

local failures, passes = 0, 0
local function ok(cond, name)
    if cond then passes = passes + 1 else failures = failures + 1; print('FAIL ' .. name) end
end

-- A fake gear -----------------------------------------------------------------------------------------------
local function make_gear(mod_dir, opts)
    opts = opts or {}
    local G = {handlers = {}, keys = {}, files = {}, texts = {}, buttons_pressed = {}, panels = {}, logs = {}}
    local registry, n = {}, 0
    G.hero_state = {gold = 100, xp = 1000, level = 5, type = 77, name = 'Aria', x = 1000, y = 1000, layer = 0, present = true}
    G.ground = {}          -- items lying around
    G.backpack, G.chest = {}, {}
    G.settings_map = {}

    local hero = {}
    function hero:gold() return G.hero_state.gold end
    function hero:experience() return G.hero_state.xp end
    function hero:level() return G.hero_state.level end
    function hero:type() return G.hero_state.type end
    function hero:name() return G.hero_state.name end
    function hero:position() return G.hero_state.x, G.hero_state.y, G.hero_state.layer end

    local gear = {version = 'test', mod = {id = mod_dir}}
    function gear.on(event, fn) G.handlers[event] = G.handlers[event] or {}; table.insert(G.handlers[event], fn) end
    gear.keys = {on = function(id, fn) G.keys[id] = fn end}
    gear.settings = {get = function(id)
        if opts.settings_throw then error('no settings') end
        return G.settings_map[id]
    end}
    function gear.read(name) return G.files[name], (G.files[name] == nil) and 'missing' or nil end
    function gear.save(name, text) G.files[name] = text; G.saves = (G.saves or 0) + 1; return true end
    gear.json = {
        encode = function(t) n = n + 1; registry['J' .. n] = t; return 'J' .. n end,
        decode = function(text) return registry[text] end,
    }
    gear.world = {
        TILE = 53.7,
        hero = function() if G.hero_state.present then return hero end end,
        items_near = function() return G.ground end,
    }
    gear.inv = {container = function(which)
        local list = which == 'backpack' and G.backpack or which == 'chest' and G.chest or nil
        if not list then return nil end
        return {items = function() return list end}
    end}
    gear.data = {object_types = {name = function(_, id) return 'creature' .. tostring(id) end}}
    local ui = {COLORS = {accent = 1}}
    function ui.rgba() return 0 end
    function ui.text(text) G.texts[#G.texts + 1] = tostring(text) end
    function ui.label(_, text) G.texts[#G.texts + 1] = tostring(text) end
    function ui.rect() end
    function ui.item_tooltip_text(item) return item.tip end
    function ui.panel(id) G.panels[id] = true; return {open = true, content_x = 10, content_y = 10, content_w = 250, content_h = 300} end
    function ui.button(id) return G.buttons_pressed[id] == true end
    function ui.dropdown(_, _, _, _, _, index) return false, index end
    function ui.scroll_begin() return {x = 0, y = 0, w = 200, h = 200, content_x = 0, content_y = 0} end
    function ui.scroll_end() end
    function ui.show_panel(id, open) G.panels[id] = open end
    function ui.panel_open(id) return G.panels[id] end
    gear.ui = ui
    G.gear = gear
    return G
end

local function load_mod(dir, opts)
    local G = make_gear(dir, opts)
    local env = setmetatable({gear = G.gear, print = function(...) G.logs[#G.logs + 1] = table.concat({...}, ' ') end}, {__index = _G})
    local chunk, err = loadfile('gear/examples/' .. dir .. '/main.lua')
    assert(chunk, err)
    setfenv(chunk, env)
    chunk()
    return G
end

local function fire(G, event, ...)
    local last
    for _, fn in ipairs(G.handlers[event] or {}) do last = fn(...) end
    return last
end

local function frames(G, seconds, step)
    step = step or 0.125
    for _ = 1, math.floor(seconds / step + 0.5) do fire(G, 'frame', step) end
end

local function joined(G) return table.concat(G.texts, '\n') end

-- manifests are plain tables
for _, dir in ipairs({'sessiontracker', 'lootlog', 'damagemeter'}) do
    local m = dofile('gear/examples/' .. dir .. '/mod.lua')
    ok(type(m) == 'table' and m.id == dir, dir .. ' manifest id')
    local ids = {}
    for _, s in ipairs(m.settings or {}) do ok(not ids[s.id], dir .. ' unique setting ' .. s.id); ids[s.id] = true end
end

-- Session tracker --------------------------------------------------------------------------------------------
do
    local G = load_mod('sessiontracker')
    fire(G, 'load')
    fire(G, 'session.start')
    fire(G, 'draw', {mode = 'game'})
    frames(G, 60)
    G.hero_state.gold = 150           -- +50
    G.hero_state.xp = 1300            -- +300
    frames(G, 1)
    G.hero_state.gold = 120           -- -30 spent
    G.hero_state.level = 6
    frames(G, 1)
    fire(G, 'kill', {by_local = true})
    fire(G, 'kill', {by_local = true})
    fire(G, 'kill', {by_local = false})
    fire(G, 'kill', {killer = {is_local = true}})
    fire(G, 'kill', nil)
    fire(G, 'kill', 'garbage')
    G.texts = {}
    fire(G, 'draw', {mode = 'game'})
    local t = joined(G)
    ok(t:find('0:01:02', 1, true), 'tracker: time played 62 s: ' .. t:sub(1, 60))
    ok(t:find('3 ', 1, true), 'tracker: 3 own kills')
    ok(t:find('50', 1, true), 'tracker: gold gained 50')
    ok(t:find('300', 1, true), 'tracker: xp gained 300')
    ok(t:find('Levels gained'), 'tracker: levels row')
    -- paused in a menu: no time
    fire(G, 'draw', {mode = 'game', escape_menu = true})
    frames(G, 10)
    G.texts = {}
    fire(G, 'draw', {mode = 'game', escape_menu = true})
    ok(joined(G):find('0:01:02', 1, true), 'tracker: no time counted in the escape menu')
    -- rates: 3 kills in 62 s is 174/h
    ok(joined(G):find('174/h', 1, true), 'tracker: kills per hour')
    ok(#G.logs == 0, 'tracker: no errors on the main path: ' .. table.concat(G.logs, '|'))
    fire(G, 'session.stop')
    local saved = G.gear.json.decode(G.files['session_totals.json'])
    ok(saved and saved.lifetime.kills == 3 and saved.lifetime.gold_gained == 50 and saved.lifetime.xp_gained == 300, 'tracker: lifetime saved')
    ok(saved and #saved.history == 1 and saved.history[1].hero == 'Aria', 'tracker: history entry')
    -- the next session adds to the lifetime
    fire(G, 'session.start')
    fire(G, 'draw', {mode = 'game'})
    frames(G, 5)
    G.hero_state.gold = 1000
    frames(G, 1)
    G.texts = {}
    fire(G, 'draw', {mode = 'game'})
    ok(joined(G):find('0:00:06', 1, true), 'tracker: second session time')
    -- short session: rate is "-"
    ok(joined(G):find('-', 1, true) ~= nil, 'tracker: short session has no rate')
    -- reset button
    G.buttons_pressed.reset = true
    fire(G, 'draw', {mode = 'game'})
    G.buttons_pressed.reset = nil
    saved = G.gear.json.decode(G.files['session_totals.json'])
    ok(saved and #saved.history == 2, 'tracker: reset moved the session to history')
end

-- Session tracker: nothing there (no hero, settings failing, bad dt) must not raise
do
    local G = load_mod('sessiontracker', {settings_throw = true})
    G.hero_state.present = false
    fire(G, 'frame', nil); fire(G, 'frame', 'x'); fire(G, 'frame', -1); fire(G, 'frame', 1e9)
    fire(G, 'draw', nil); fire(G, 'draw', {mode = 'menus'}); fire(G, 'draw', {mode = 'game'})
    fire(G, 'session.stop'); fire(G, 'unload')
    G.files['session_totals.json'] = 'garbage'
    fire(G, 'load')
    ok(#G.logs == 0, 'tracker: no errors logged on empty data: ' .. table.concat(G.logs, '|'))
    G.hero_state.present = true
    G.keys.panel()
    ok(true, 'tracker: survived')
end

-- Loot log ---------------------------------------------------------------------------------------------------
do
    local G = load_mod('lootlog')
    fire(G, 'load')
    fire(G, 'session.start')
    local old = {id = 5, type = 1, type_name = 'OldSword', quality = 'rare', category = 'weapon'}
    G.ground = {old}
    frames(G, 1)                                           -- baseline: not logged
    fire(G, 'drops.roll', {creature = 12, level = 3})
    local sword = {id = 6, type = 2, type_name = 'Blade', quality = 'magic', category = 'weapon', tip = '\\gffffffff     19e\\cffefefef Blade of Haste \n\\cffa020f0Dunkelelf\nPreis: 320\n\\gffffffff000000cd'}
    local plain = {id = 7, type = 3, type_name = 'Rag', quality = 'normal', category = 'armor'}
    local unique = {id = 8, type = 4, type_name = 'Excalibur', quality = 'unique', category = 'weapon'}
    G.ground = {old, sword, plain, unique}
    frames(G, 1)
    ok(#G.logs == 0, 'loot: no errors on the main path: ' .. table.concat(G.logs, '|'))
    -- read back through a save
    fire(G, 'session.stop')
    local data = G.gear.json.decode(G.files['lootlog.json'])
    ok(data and #data.entries == 2, 'loot: two worthwhile drops logged (plain and baseline skipped): ' .. tostring(data and #data.entries))
    local names = {}
    for _, e in ipairs(data and data.entries or {}) do names[e.name] = e end
    ok(names['Blade of Haste'] and names['Blade of Haste'].src == 'creature12' and names['Blade of Haste'].kind == 'drop', 'loot: name from tooltip, source from drops.roll')
    ok(names['Excalibur'] and names['Excalibur'].quality == 'unique', 'loot: falls back to the type name')
    -- pickup: the sword moves into the backpack
    fire(G, 'session.start')
    G.ground = {sword}
    frames(G, 1)
    G.ground = {}
    G.backpack = {sword}
    frames(G, 1)
    -- an item that was in the chest and moves to the backpack is ignored; a bought one is "acquired"
    local moved = {id = 20, type = 9, type_name = 'Moved', quality = 'rare', category = 'armor'}
    G.chest = {moved}
    frames(G, 1)
    G.chest = {}
    G.backpack = {sword, moved}
    frames(G, 1)
    local bought = {id = 21, type = 10, type_name = 'Bought', quality = 'rare', category = 'shield'}
    G.backpack = {sword, moved, bought}
    frames(G, 1)
    fire(G, 'unload')
    data = G.gear.json.decode(G.files['lootlog.json'])
    local kinds = {}
    for _, e in ipairs(data.entries) do if e.s == 2 then kinds[e.name] = e.kind end end
    ok(kinds['Blade of Haste'] == 'pickup', 'loot: pickup after being on the ground: ' .. tostring(kinds['Blade of Haste']))
    ok(kinds['Moved'] == nil, 'loot: chest to backpack ignored')
    ok(kinds['Bought'] == 'acquired', 'loot: acquired')
    -- the panel draws the entries, newest first; the log survives a restart
    local G2 = load_mod('lootlog')
    G2.files['lootlog.json'] = G.files['lootlog.json']
    G2.gear.json.decode = G.gear.json.decode
    fire(G2, 'load')
    fire(G2, 'draw', {mode = 'game'})
    ok(joined(G2):find('Blade', 1, true), 'loot: reloaded entries are drawn')
    ok(joined(G2):find('entries', 1, true), 'loot: count line')
end

-- Loot log: hostile data
do
    local G = load_mod('lootlog', {settings_throw = true})
    G.hero_state.present = false
    fire(G, 'frame', 1); fire(G, 'draw', {mode = 'game'})
    G.hero_state.present = true
    G.ground = {'junk', {}, {id = 1}, {id = 2, type = 1, quality = 'weird', category = 5}}
    G.backpack = {false, {id = 'x'}}
    fire(G, 'session.start')
    frames(G, 2)
    fire(G, 'drops.roll', nil); fire(G, 'drops.roll', {creature = 'a'})
    fire(G, 'draw', {mode = 'game'}); fire(G, 'draw', nil)
    G.files['lootlog.json'] = 'nonsense'
    fire(G, 'load')
    ok(#G.logs == 0, 'loot: no errors logged on hostile data: ' .. table.concat(G.logs, '|'))
end

-- Damage meter -----------------------------------------------------------------------------------------------
do
    local G = load_mod('damagemeter')
    frames(G, 1)                                          -- learns the hero type (77)
    fire(G, 'damage', {damage = 100, victim_type = 5, attacker_type = 77})
    fire(G, 'damage', {damage = 300, victim_type = 5, attacker_type = 77})
    fire(G, 'damage', {damage = 40, victim_type = 77, attacker_type = 6})
    fire(G, 'damage', {damage = 10, victim_type = 77, attacker_type = -1})
    fire(G, 'damage', {damage = 999, victim_type = 5, attacker_type = 6})      -- monsters among themselves: ignored
    fire(G, 'damage', {damage = 999, victim_type = 77, attacker_type = 77})    -- ambiguous: ignored
    fire(G, 'damage', {damage = 'x'}); fire(G, 'damage', nil); fire(G, 'damage', {})
    ok(fire(G, 'damage', {damage = 5, victim_type = 5, attacker_type = 77}) == nil, 'damage: never changes the damage')
    frames(G, 3)
    fire(G, 'kill', {by_local = true})
    G.texts = {}
    fire(G, 'draw', {mode = 'game'})
    local t = joined(G)
    ok(t:find('405  (3 hits)', 1, true), 'damage: dealt 405 in 3 hits: ' .. t)
    ok(t:find('50  (2 hits)', 1, true), 'damage: taken 50 in 2 hits')
    ok(t:find('300 / 40', 1, true), 'damage: biggest hits')
    ok(t:find('creature5', 1, true), 'damage: target named')
    ok(t:find('environment', 1, true), 'damage: environment source')
    -- the fight ends after the timeout and goes to the history; DPS of the finished fight stays visible
    frames(G, 8)
    G.texts = {}
    fire(G, 'draw', {mode = 'game'})
    t = joined(G)
    ok(t:find('Fight (last)', 1, true), 'damage: fight ended')
    -- rolling window empties
    local dps_line_ok = t:find('Last 10 s', 1, true)
    ok(dps_line_ok, 'damage: rolling line')
    -- reset
    G.buttons_pressed.reset = true
    fire(G, 'draw', {mode = 'game'})
    G.buttons_pressed.reset = nil
    G.texts = {}
    fire(G, 'draw', {mode = 'game'})
    ok(joined(G):find('0  (0 hits)', 1, true), 'damage: reset clears')
    G.keys.reset()
    G.keys.panel()
    ok(#G.logs == 0, 'damage: no errors on the main path: ' .. table.concat(G.logs, '|'))
end

-- Damage meter: no hero, bad data
do
    local G = load_mod('damagemeter', {settings_throw = true})
    G.hero_state.present = false
    fire(G, 'frame', 0.1)
    fire(G, 'damage', {damage = 10, victim_type = 1, attacker_type = 2})
    fire(G, 'draw', {mode = 'game'})
    G.hero_state.present = true
    G.hero_state.type = nil
    fire(G, 'frame', 0.1)
    fire(G, 'damage', {damage = 10, victim_type = 1, attacker_type = 2})
    fire(G, 'draw', {mode = 'game'})
    fire(G, 'frame', nil); fire(G, 'frame', 'x'); fire(G, 'draw', nil)
    ok(#G.logs == 0, 'damage: no errors logged on missing data: ' .. table.concat(G.logs, '|'))
end

print(('%d passed, %d failed'):format(passes, failures))
os.exit(failures == 0 and 0 or 1)
