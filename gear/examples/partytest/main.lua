-- A pinpointed self test of gear.party (companion heroes in single player):
--   Sacred.exe -gear-auto -gear-load=N -gear-mods=partytest -gear-instance=NAME
-- Use a copy of the game folder: it writes a savegame (config save_slot). Results: PASS/FAIL lines in SacredGear.log
-- and GearAutomation\NAME\partytest.json, screenshots party-*.bmp.
if not gear.auto then return end
local A = gear.auto
local party = gear.party
local passed, failed, results = 0, 0, {}
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    local line = ('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or '')
    results[#results + 1] = line
    print(line)
end

local switches = {}
gear.on('party.control', function(from, to) switches[#switches + 1] = {from and from.id, to and to.id} end)

local function finish()
    A.write('partytest.json', gear.json.encode({passed = passed, failed = failed, results = results}))
    print(('%d passed, %d failed'):format(passed, failed))
    if gear.config.quit then A.quit(failed == 0 and 0 or 1) end
end

-- The nearest living enemy of `hero` within `radius`.
local function enemy_near(hero, radius)
    local x, y, layer = hero:position()
    for _, c in ipairs(gear.world.creatures_near(x, y, radius, {layer = layer})) do
        if not c:is_dead() and hero:is_enemy(c) then return c end
    end
end

local function moved(ax, ay, bx, by, min)
    return ax and bx and (math.abs(bx - ax) + math.abs(by - ay)) > min
end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then check('game', false) return finish() end
    gear.wait(3)
    local ok, why = party.available()
    check('available', ok, why)
    local main = party.leader()
    check('leader is the hero', main ~= nil and main == gear.world.hero())
    check('one hero', #party.heroes() == 1, #party.heroes())
    check('portrait of the hero', main:portrait() ~= nil, main:portrait())

    -- Recruit with the experience of a level 10 or so hero.
    local comp, err = party.recruit('darkelf', {experience = 60000})
    check('recruit', comp ~= nil, err)
    if not comp then return finish() end
    check('companion id', comp.id >= party.FIRST_ID and comp.id <= party.LAST_ID, comp.id)
    check('companion class', comp:class() == 'darkelf', comp:class())
    check('companion levelled', (comp:level() or 0) > 5, comp:level())
    check('two heroes, leader first', #party.heroes() == 2 and party.heroes()[1] == main)
    check('companions()', #party.companions() == 1 and party.companions()[1] == comp)
    check('world changes still allowed', gear.world.can_change())
    local arts = comp:abilities() or {}
    check('companion knows an art', #arts >= 1 and arts[1].art, arts[1] and arts[1].id)

    -- Orders to the companion while the main hero is controlled.
    gear.wait(1)
    local cx, cy = comp:position()
    check('order_move', comp:order_move(cx + 150, cy - 100))
    gear.wait(2.5)
    local nx, ny = comp:position()
    check('companion walked', moved(cx, cy, nx, ny, 60), ('%s,%s -> %s,%s'):format(cx, cy, tostring(nx), tostring(ny)))

    -- Switch control.
    local gold = main:gold() or 0
    check('control', party.control(comp))
    gear.wait(1)
    check('hero() is the companion', gear.world.hero() == comp)
    check('leader() is the companion', party.leader() == comp and party.heroes()[1] == comp)
    check('party.control event', #switches == 1 and switches[1][1] == main.id and switches[1][2] == comp.id)
    check('gold went along', comp:gold() == gold and main:gold() == 0,
        ('%s -> %s / %s'):format(gold, tostring(comp:gold()), tostring(main:gold())))
    A.screenshot('party-1-control')
    -- The old hero is now an AI hero: it takes orders.
    local ox, oy, ol = main:position()
    check('order to the former leader', main:order_move(ox - 120, oy + 120))
    gear.wait(2.5)
    local px, py = main:position()
    check('former leader walked', moved(ox, oy, px, py, 60))
    -- The world keeps ticking: a creature nearby moves within a few seconds.
    local watch = {}
    for _, c in ipairs(gear.world.creatures_near(px, py, 1500, {layer = ol})) do
        if not c:is_hero() then
            local x, y = c:position()
            watch[#watch + 1] = {c, x, y}
        end
    end
    gear.wait(5)
    local n = 0
    for _, w in ipairs(watch) do
        local x, y = w[1]:position()
        if x and (x ~= w[2] or y ~= w[3]) then n = n + 1 end
    end
    check('the world keeps ticking', n > 0 or #watch == 0, n .. ' of ' .. #watch .. ' moved')
    A.press('i') gear.wait(1.5) A.screenshot('party-2-inventory') A.press('i') gear.wait(0.5)

    -- Combat: the former leader (an AI hero now) uses its combat art on an enemy; experience is shared.
    party.share_experience(1, 0)
    local foe = enemy_near(main, 1600)
    check('an enemy nearby', foe ~= nil)
    if foe then
        local fx, fy = main:position()
        foe:teleport(fx + 120, fy - 120)
        gear.wait(0.5)
        local hp0 = foe:hp()
        local art = (main:abilities() or {})[1]
        check('main knows an art', art ~= nil)
        local e1, e2 = main:experience(), comp:experience()
        for _ = 1, 8 do
            if foe:is_dead() then break end
            local a = art and main:ability(art.id)
            if a and a.ready then main:order_ability(art.id, foe) else main:order_attack(foe) end
            gear.wait(2)
        end
        check('the enemy took damage', foe:is_dead() or (foe:hp() or hp0) < hp0, ('%s -> %s'):format(hp0, tostring(foe:hp())))
        local a = art and main:ability(art.id)
        check('art regeneration known', a ~= nil and a.regen_max > 0, a and a.regen_max)
        if foe:is_dead() then
            gear.wait(1)
            local d1, d2 = main:experience() - e1, comp:experience() - e2
            check('experience shared', d1 > 0 and d2 > 0, d1 .. ' / ' .. d2)
        end
    end
    party.share_experience(0)

    -- Save while the companion is controlled, load, the party is still there.
    local slot = gear.config.save_slot
    check('save', gear.game.save(slot, 'partytest'))
    gear.wait_until(function() return not gear.game.save_pending() end, 30)
    gear.wait(1)
    check('load', gear.game.load(slot))
    gear.wait(3)
    A.wait_for_game(120)
    gear.wait(4)
    local heroes = party.heroes()
    check('after load: two heroes', #heroes == 2, #heroes)
    local back = party.leader()
    check('after load: a leader', back ~= nil, back and back.id)
    local other
    for _, h in ipairs(heroes) do if h ~= back then other = h end end
    check('after load: control the other', other ~= nil and party.control(other))
    gear.wait(1)
    check('after load: switched', other ~= nil and gear.world.hero() == other)
    if other then
        local sx, sy = other:position()
        local s = A.state()
        A.click(math.floor(s.width / 2) + 160, math.floor(s.height / 2) + 90, 'lbutton')
        gear.wait(2.5)
        local tx, ty = other:position()
        check('after load: the controlled hero walks by mouse', moved(sx, sy, tx, ty, 40))
    end
    A.screenshot('party-3-loaded')

    -- Back to the main hero (id 1) and dismiss the companion.
    local first = gear.world.object(1)
    check('control the main hero', first ~= nil and party.control(first))
    gear.wait(1)
    check('dismiss refuses the leader', not party.dismiss(first))
    local companion = party.companions()[1]
    check('dismiss', companion ~= nil and party.dismiss(companion))
    gear.wait(1)
    check('one hero again', #party.heroes() == 1)
    A.screenshot('party-4-done')
    finish()
end)
