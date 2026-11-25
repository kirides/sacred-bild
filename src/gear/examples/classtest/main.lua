if not gear.auto then return end
local A = gear.auto
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('S1 %s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or ''))
end
local function hex(s) return (s:gsub('.', function(c) return ('%02x'):format(c:byte()) end)) end
local usable_calls = 0
gear.on('rune.usable', function(t, ab, vanilla) usable_calls = usable_calls + 1 end)

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then A.quit(1) return end
    gear.wait(3)
    -- hero records
    for t = 1, 9 do
        local r = gear.classes.hero_record and gear.classes.hero_record(t)
        if t == 1 then print('S1 HERO ' .. t .. ' ' .. tostring(r and hex(r))) end
    end
    local sk = gear.classes.hero_skills('seraphim')
    check('hero_skills', sk and #sk.start == 2 and #sk.available >= 10, sk and table.concat(sk.available, ','))
    local ok_set, why_set = gear.classes.set_hero_skills('seraphim', {7, 9}, {7, 9, 1, 2})
    local sk2 = gear.classes.hero_skills(1)
    check('set_hero_skills', ok_set and sk2.start[1] == 7 and #sk2.available == 4, why_set)
    gear.classes.restore_hero_skills()
    local sk3 = gear.classes.hero_skills(1)
    check('restore_hero_skills', sk3.start[1] == sk.start[1] and #sk3.available == #sk.available)
    local cat = gear.classes.catalog()
    check('catalog', #cat > 150, #cat)
    local runes = 0
    for _, e in ipairs(cat) do if #e.runes > 0 then runes = runes + 1 end end
    check('catalog has rune items', runes > 20, runes)
    local by = {}
    for _, e in ipairs(cat) do by[e.id] = e end
    check('tier gladiator art for seraphim is B', by[1059] and by[1059].tiers[1] == 'B', by[1059] and by[1059].tiers[1])
    check('tier of 1022 for seraphim is C', by[1022].tiers[1] == 'C')
    check('tier of spell is A', by[5].tiers[2] == 'A')
    print('S1 RUNES 1059=' .. table.concat(by[1059].runes, ',') .. ' 5=' .. table.concat(by[5].runes, ','))

    -- alias
    local base = gear.data.abilities[1059]
    local eb = base.effect_base
    local rec, err = gear.classes.copy_ability(2001, 1059, {effect_base = eb * 3 + 1, effect_per_level = 5})
    check('copy_ability', rec ~= nil, err)
    check('alias id', rec and rec.id == 2001 and rec.base == 1059 and rec.kind == 'combat_art', rec and rec.id)
    check('alias value', rec and math.abs(rec.effect_base - (eb * 3 + 1)) < 0.01, rec and rec.effect_base)
    check('base unchanged', gear.data.abilities[1059].effect_base == eb)
    check('ids has alias', (function() for _, id in ipairs(gear.data.abilities:ids()) do if id == 2001 then return true end end end)())
    local tid = gear.text.add({en = 'Venom Strike', de = 'Giftschlag'}); check('text add', tid ~= nil and tid >= 0x100000, tid)
    check('text get', gear.text.get(tid) ~= nil, gear.text.get(tid))
    if rec then rec.name_text = tid end
    check('name text', rec and gear.text.get(rec.name_text), rec and gear.text.get(rec.name_text))
    local r2, e2 = gear.classes.copy_ability(101, 5, {value0_base = 77})
    check('spell copies are refused', r2 == nil and e2 ~= nil, e2)
    check('bad base', select(2, gear.classes.copy_ability(2003, 9999)) ~= nil)
    check('bad alias id', select(2, gear.classes.copy_ability(5, 1059)) ~= nil)

    -- use on a gladiator
    local gla = gear.party.recruit('gladiator', {experience = 120000})
    check('recruited', gla ~= nil)
    if gla then
        local ok, why = gla:learn(2001, 1)
        check('learn alias', ok, why)
        local ab = gla:ability(2001)
        check('ability known with alias id', ab ~= nil and ab.id == 2001, ab and ab.id)
        local foe
        local x, y, layer = gla:position()
        for _, c in ipairs(gear.world.creatures_near(x, y, 3500, {layer = layer})) do
            if not c:is_dead() and gla:is_enemy(c) and not c:is_hero() then foe = c break end
        end
        if foe then
            foe:teleport(x + 130, y - 130)
            gear.wait(0.4)
            local o = gla:order_ability(2001, foe)
            local last, trace = nil, {}
            for i = 1, 20 do
                gear.wait(0.15)
                local act = gla:action()
                if act ~= last then trace[#trace + 1] = ('%.1f:%s'):format(i * 0.15, tostring(act)); last = act end
            end
            check('alias art ran', o and table.concat(trace, ' '):find('ability') ~= nil, table.concat(trace, ' '))
        else
            check('foe', false)
        end
        check('forget', gla:forget(2001))
        check('forgotten', gla:ability(2001) == nil)
    end
    gear.classes.remove_ability(2001)
    check('removed', gear.data.abilities[2001] == nil)
    print(('S1 %d passed %d failed, usable calls %d'):format(passed, failed, usable_calls))
    A.quit(failed == 0 and 0 or 1)
end)
