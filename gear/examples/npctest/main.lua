-- Self test of gear.npc in a loaded savegame (-gear-auto -gear-load=N -gear-mods=npctest): the model of a creature near the hero is used for an
-- NPC with a name of its own, its dialog is opened and a button pressed with a real click, and a routine moves it. Results go to the log
-- ("npctest: PASS|FAIL ...") and to result.json; screenshots npctest-*.png.
local A = gear.auto
local N = gear.npc

local results = {}
local function check(name, ok, detail)
    results[#results + 1] = {name = name, ok = ok and true or false, detail = detail}
    gear.log(('npctest: %s %s %s'):format(ok and 'PASS' or 'FAIL', name, detail or ''))
end

local pressed = {}
N.on('button', function(e) pressed[#pressed + 1] = ('%s/%s/%s'):format(e.npc, e.page, tostring(e.option)) end)
local talked = 0
local arrived = 0
N.on('arrived', function() arrived = arrived + 1 end)

local function finish()
    local failed = 0
    for _, r in ipairs(results) do if not r.ok then failed = failed + 1 end end
    A.write('result.json', gear.json.encode(results))
    A.quit(failed == 0 and 0 or 1)
end

gear.task(function()
    A.wait_for_menu(120)
    A.wait_for_game(180)
    gear.wait(3)
    local hero = gear.world.hero()
    local hx, hy, layer = hero:position()

    -- a model: the type of the nearest other creature
    local model
    for _, c in ipairs(gear.world.creatures_near(hx, hy, 900, {limit = 30})) do
        if c.id ~= hero.id then model = c:type() break end
    end
    check('a creature type to borrow the model of', model ~= nil)
    if not model then return finish() end

    -- the clock
    local minutes, hour, minute = gear.world.clock()
    check('the game clock reads', minutes ~= nil and hour == math.floor(minutes / 60) and minute == minutes % 60, tostring(minutes))

    local p1x, p1y = hx + 150, hy
    local p2x, p2y = hx - 150, hy
    local function hhmm(m) return ('%02d:%02d'):format(math.floor(m / 60), m % 60) end
    local now = minutes or 0
    -- the routine: stand at p1 from now on, at p2 two minutes after the clock's minute (the day passes at a few game minutes per second)
    N.define('npctest.keeper', {
        type = model,
        name = {en = 'Keeper Tessel', de = 'Wächterin Tessel'},
        talk_range = 400,
        walk_radius = 5000,
        routine = {{time = hhmm(now), x = p1x, y = p1y}, {time = hhmm((now + 5) % 1440), x = p2x, y = p2y}},
        dialog = {
            start = 'greet',
            pages = {
                greet = {
                    text = 'Welcome, traveller. Can I interest you in a pet, or in some trade?',
                    options = {
                        {id = 'pet', label = 'Buy a pet', on = function(e) talked = talked + 1 end, close = true},
                        {id = 'more', label = 'Tell me more', go = 'more'},
                        {id = 'bye', label = 'Goodbye', close = true},
                    },
                },
                more = {text = function() return 'I keep the gate.' end, options = {{id = 'back', label = 'Back', go = 'greet'}}},
            },
        },
    })

    local c, err = N.spawn('npctest.keeper', hx - 150, hy, {layer = layer})
    check('spawn', c ~= nil, tostring(err))
    if not c then return finish() end
    gear.wait(1)
    check('name text set', (c:name_text() or 0) ~= 0, tostring(c:name_text()))
    check('display_name is the NPC name', c:display_name() == 'Keeper Tessel' or c:display_name() == 'Wächterin Tessel', tostring(c:display_name()))
    check('of() finds the kind', N.of(c) == 'npctest.keeper')
    check('name key', c:name_key() == 'npc:npctest.keeper', tostring(c:name_key()))
    local raw = A.object_bytes(c.id, 0x3C, 4)
    local t = c:name_text()
    local want = ('%02x%02x%02x%02x'):format(t % 256, math.floor(t / 256) % 256, math.floor(t / 65536) % 256, math.floor(t / 16777216) % 256)
    check('+0x3C holds the id', raw and raw:lower() == want, tostring(raw) .. ' vs ' .. want)
    check('list has it', #N.list('npctest.keeper') == 1)
    A.key_down('alt')
    gear.wait(0.5)
    A.screenshot('npctest-1-spawned')
    A.key_up('alt')

    -- the routine: the NPC walks to p1 (hero is near), then to p2 when the clock passes the minute
    local function dist(px, py)
        local cx, cy = c:position()
        return math.sqrt((cx - px) ^ 2 + (cy - py) ^ 2)
    end
    -- walking shows as positions between the start and the point
    local start_d, walking = dist(p1x, p1y), false
    local clock0 = gear.world.clock()
    local t0 = gear.time()
    local ok1 = gear.wait_until(function()
        local d = dist(p1x, p1y)
        if d > 80 and d < start_d - 60 then walking = true end
        return d < 60
    end, 60)
    local clock1 = gear.world.clock()
    gear.log(('npctest: clock %s -> %s in %.1f s'):format(tostring(clock0), tostring(clock1), gear.time() - t0))
    check('reached point 1', ok1, ('%.0f of %.0f'):format(dist(p1x, p1y), start_d))
    check('it walked there (seen on the way)', walking)
    A.screenshot('npctest-4-point1')

    -- a dialog and a real click on its first button
    check('talk opens', N.talk(c) == true)
    gear.wait(1)
    A.screenshot('npctest-2-dialog')
    local x, y, w, h = A.widget('opt1', 'npctest')
    check('first button drawn', x ~= nil)
    if x then
        A.ui_click(x + w / 2, y + h / 2)
        gear.wait(0.6)
    end
    check('button handler ran', talked == 1 and pressed[1] == 'npctest.keeper/greet/pet', table.concat(pressed, ','))
    check('dialog closed by the option', N.dialog_state() == nil)
    N.talk(c)
    gear.wait(0.5)
    x, y, w, h = A.widget('opt2', 'npctest')
    if x then A.ui_click(x + w / 2, y + h / 2) gear.wait(0.6) end
    check('goto page', N.dialog_state() and N.dialog_state().page == 'more')
    A.screenshot('npctest-3-page2')
    N.close()

    local ok2 = gear.wait_until(function() return dist(p2x, p2y) < 60 end, 150)
    check('moved on to point 2 when the time came', ok2, ('%.0f'):format(dist(p2x, p2y)))
    check('arrived event', arrived >= 1, tostring(arrived))
    A.screenshot('npctest-5-point2')

    -- a hero far away: the NPC is put at its point at once (walk_radius 50)
    N.define('npctest.far', {type = model, name = 'Far Keeper', walk_radius = 50, routine = {{time = '00:00', x = hx - 300, y = hy - 100}}})
    local f = N.spawn('npctest.far', hx + 120, hy + 120, {layer = layer})
    check('spawn far', f ~= nil)
    if f then
        gear.wait(2.5)
        local fx, fy = f:position()
        check('put at its point at once', math.abs(fx - (hx - 300)) < 40 and math.abs(fy - (hy - 100)) < 40, ('%d,%d'):format(fx, fy))
        check('two kinds listed', #N.list() == 2)
    end
    finish()
end)
