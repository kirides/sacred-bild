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

    -- A combo: the companion gets one (the combo master's slot 0) and uses it on an enemy next to it.
    local art = (comp:abilities() or {})[1]
    check('set_combo', art ~= nil and comp:set_combo(0, {art.id, art.id}))
    local combo = (comp:combos() or {})[1]
    check('combos()', combo ~= nil and combo.id == 0x200 and #combo.arts == 2 and combo.total > 0, combo and combo.total)
    local foe2 = enemy_near(comp, 3000)
    if combo and foe2 then
        local fx, fy = foe2:position()
        for _, o in ipairs({{120, 120}, {250, 250}, {-150, 0}, {0, -150}, {150, 0}, {0, 150}}) do
            if comp:teleport(fx + o[1], fy + o[2]) then break end
        end
        local ready = gear.wait_until(function() local k = (comp:combos() or {})[1] return k and k.ready end, combo.total + 5)
        check('combo regenerated', ready)
        check('order the combo', comp:order_ability(combo.id, foe2))
        gear.wait(1.5)
        local k = (comp:combos() or {})[1]
        check('the combo ran (regenerates again)', k ~= nil and not k.ready, k and k.charge)
    end
    check('set_combo clears', comp:set_combo(0, {}) and #(comp:combos() or {}) == 0)

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

    -- Hero files: write the controlled hero to one, bring it into the party as a copy.
    local spec, xerr = party.export_hero('partytest')
    check('export_hero', spec == 'mod:heroes/partytest.pax', xerr or spec)
    local found
    for _, f in ipairs(party.hero_files()) do if f.file == spec then found = f end end
    check('hero_files lists it', found ~= nil and found.class ~= nil and found.level >= 1,
        found and (found.class .. ' ' .. found.level))
    local copy, ierr = found and party.recruit_hero(found.file)
    check('recruit_hero', copy ~= nil, ierr)
    if copy then
        local played = party.leader()
        check('the copy has the class and level', copy:class() == played:class() and copy:level() == played:level(),
            tostring(copy:class()) .. ' ' .. tostring(copy:level()))
        check('the copy has abilities', #(copy:abilities() or {}) == #(played:abilities() or {}))
        check('three heroes', #party.heroes() == 3)
    end

    -- Mercenaries: the save's own follow the controlled hero and take orders.
    local mercs = party.mercenaries()
    check('mercenaries()', #mercs >= 1, #mercs)
    if mercs[1] then
        local m = mercs[1]
        local x, y = m:position()
        check('a mercenary takes a move order', m:order_move(x + 80, y + 40))
        check('a mercenary has an action', m:action() ~= nil)
    end

    -- Pickups of a companion land in its backpack, also after the inventory window was open while control changed hands
    -- (the window's mark on the hero's container moves with control; left behind it sent pickups to the cursor's hand and the items vanished).
    do
        local function pickup_into(hero, label)
            local x, y, layer = hero:position()
            local item = gear.world.spawn(5169, x - 60, y, {layer = layer})
            gear.wait(0.5)
            local c = gear.inv.container(hero.id)
            local before = c and #c:items() or -1
            local ok, why = hero:pickup(item)
            gear.wait(1)
            check(label, ok and c and #c:items() == before + 1, tostring(why) .. ' ' .. before .. ' -> ' .. (c and #c:items() or -1))
        end
        local lead = gear.world.object(1)
        local other = party.companions()[1] or lead
        party.control(lead) gear.wait(1)
        A.press(0x49) gear.wait(1.5)            -- the inventory window opens for the leader
        party.control(other) gear.wait(1)       -- control moves with the window open
        A.press(0x49) gear.wait(1.5)            -- and it closes
        pickup_into(other, 'a companion picks up into its own backpack after the window was open at the switch')
        party.control(lead) gear.wait(1)
        pickup_into(lead, 'the leader picks up into the backpack afterwards')
    end

    -- Back to the main hero (id 1) and dismiss the companion.
    local first = gear.world.object(1)
    check('control the main hero', first ~= nil and party.control(first))
    gear.wait(1)
    check('dismiss refuses the leader', not party.dismiss(first))
    local companion = party.companions()[1]
    check('dismiss', companion ~= nil and party.dismiss(companion))
    gear.wait(1)
    check('one companion fewer', #party.heroes() == (copy and 2 or 1))
    A.screenshot('party-4-done')
    finish()
end)
