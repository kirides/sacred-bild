-- A self test of the drop hooks (gear.drops), for runs without a player:
--
--     Sacred.exe -gear-auto -gear-load=1 -gear-mods=droptest -gear-instance=dropstest
--
-- Loads savegame 1, finds a creature near the hero whose loot function drops equipment, and has the engine run its loot function
-- `rolls` times per phase through gear.drops.simulate: with no setting (the vanilla counters), with a doubled chance, no chance,
-- rarity weights, a veto, a replaced item type, the fair generator (also seeded twice for the same outcome) and a Lua generator.
-- The items the rolls make stay in the world: use a copy of the game folder. Exit code 0 when every check passed.

if not gear.auto then
    print('no automation (start the game with -gear-auto): nothing to do')
    return
end

local cfg = gear.config
local A = gear.auto
local drops = gear.drops
local results = {passed = 0, failed = 0}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
    return ok
end

local function finish()
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    A.write('droptest.json', gear.json.encode(results))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end

local function hist(s)
    local parts = {}
    for q = 0, 15 do
        if s.quality[q] > 0 then parts[#parts + 1] = ('q%d=%d'):format(q, s.quality[q]) end
    end
    return table.concat(parts, ' ')
end

-- Runs `n` loot rolls of `creature` and returns the counters of just those.
local function run(creature, n, bonus, forced)
    drops.reset_stats()
    local ok, why = drops.simulate(creature, n, bonus or 0, forced)
    if not ok then print('simulate failed: ' .. tostring(why)); return nil end
    local waited = 0
    while true do
        local done, queued = drops.simulation_progress()
        if done >= queued then break end
        gear.wait(0.05)
        waited = waited + 0.05
        if waited > 120 then print('simulation timed out'); return nil end
    end
    gear.wait(0.1)
    return drops.stats()
end

local function describe(tag, s)
    if not s then return end
    print(('%s: rolls %d forced %d vetoed %d picks %d overridden %d rarity_draws %d rand_calls %d lua_draws %d | %s'):format(
        tag, s.rolls, s.forced, s.vetoed, s.picks, s.overridden, s.rarity_draws, s.rand_calls, s.lua_draws, hist(s)))
end

local function share(s, lo, hi)
    local n = 0
    for q = lo, hi do n = n + s.quality[q] end
    return s.picks > 0 and n / s.picks or 0
end

local function body()
    check('main menu', A.wait_for_menu(cfg.timeout_menu))
    if not A.wait_for_game(cfg.timeout_game) then
        check('game with a hero', false)
        return finish()
    end
    gear.wait(3)
    local hero = gear.world.hero()
    local x, y, layer = hero:position()
    check('world changes allowed', gear.world.can_change())
    local table0 = drops.rarity_table()
    check('rarity table loaded', #table0 > 0, #table0 .. ' rows')
    for _, e in ipairs(table0) do print(('rarity id %d quality %d weight %d'):format(e.id, e.quality, e.weight)) end

    -- A creature whose loot function drops equipment.
    local creature
    for _, c in ipairs(gear.world.creatures_near(x, y, 4000, {layer = layer})) do
        if c.id ~= hero.id then
            local s = run(c.id, 200)
            if s and s.picks > 0 then creature = c; describe('candidate ' .. c.id .. ' ' .. tostring(c:type_name()), s); break end
        end
    end
    if not creature then
        check('a creature that drops equipment', false)
        return finish()
    end
    check('creature found', true, creature.id)
    local cx, cy, clayer = creature:position()
    creature = creature.id

    local N = cfg.rolls
    -- 1. Vanilla: nothing in use.
    local base = run(creature, N)
    describe('vanilla', base)
    check('vanilla: rolls counted', base and base.rolls == N, base and base.rolls)
    check('vanilla: equipment drops', base and base.picks > 10, base and base.picks)
    check('vanilla: the rarity draws ran through the redirects', base and base.rarity_draws >= base.picks, base and base.rarity_draws)
    check('vanilla: the game generator answers every draw', base and base.rand_calls == 0 and base.lua_draws == 0)
    check('vanilla: nothing vetoed or replaced', base and base.vetoed == 0 and base.overridden == 0)
    local t = drops.rarity_table()
    local same = #t == #table0
    for i, e in ipairs(t) do same = same and e.weight == e.vanilla and e.weight == table0[i].weight end
    check('vanilla: the rarity table is as loaded', same)

    -- 2. Chance x2, then 0.
    drops.chance_scale(2)
    gear.wait(0.2)
    local twice = run(creature, N)
    describe('chance x2', twice)
    check('chance x2: more equipment drops', twice and base and twice.picks > base.picks * 1.5, twice and twice.picks .. ' vs ' .. base.picks)
    drops.chance_scale(0)
    gear.wait(0.2)
    local none = run(creature, N)
    describe('chance 0', none)
    check('chance 0: no drops at all', none and none.rolls == N and none.picks == 0, none and none.picks)
    drops.chance_scale(nil)

    -- 2b. Authored drops (a forced item type) are not a chance: the scale leaves them alone, also 0 (potion_small is type 5169).
    local function potions()
        local n = 0
        for _, it in pairs(gear.world.items_near(cx, cy, 3000, {layer = clayer, limit = 100000})) do
            if it.type == 5169 then n = n + 1 end
        end
        return n
    end
    local p0 = potions()
    local forced0 = run(creature, 100, 0, 5169)
    local p1 = potions()
    drops.chance_scale(0)
    gear.wait(0.2)
    local forced1 = run(creature, 100, 0, 5169)
    local p2 = potions()
    drops.chance_scale(3)
    gear.wait(0.2)
    local forced2 = run(creature, 100, 0, 5169)
    local p3 = potions()
    drops.chance_scale(nil)
    gear.wait(0.2)
    print(('forced drops of type 5169: vanilla +%d, chance 0 +%d, chance 3 +%d (100 rolls each; rolls %d forced %d)'):format(p1 - p0, p2 - p1, p3 - p2,
        forced0 and forced0.rolls or -1, forced0 and forced0.forced or -1))
    check('forced rolls are counted as authored', forced0 and forced0.forced == 100)
    check('authored drops: they happen', p1 - p0 >= 50, p1 - p0)
    check('authored drops: a chance scale of 0 does not stop them', math.abs((p2 - p1) - (p1 - p0)) <= 10, (p2 - p1) .. ' vs ' .. (p1 - p0))
    check('authored drops: a chance scale of 3 does not multiply them', math.abs((p3 - p2) - (p1 - p0)) <= 10, (p3 - p2) .. ' vs ' .. (p1 - p0))

    -- 3. Rarity weights: the plain and the lower qualities.
    drops.rarity_weights({[1] = 25, [2] = 25, [3] = 25, [4] = 25})
    gear.wait(0.2)
    local tw = drops.rarity_table()
    local ok_w = true
    for _, e in ipairs(tw) do
        if e.quality >= 1 and e.quality <= 4 then ok_w = ok_w and e.weight == math.min(65535, math.floor(e.vanilla * 25 + 0.5)) end
    end
    check('rarity weights: the table holds vanilla x 25 for qualities 1 to 4', ok_w)
    local rare = run(creature, N)
    describe('qualities 1-4 x25', rare)
    check('rarity weights: the lower qualities are much more common', rare and base and share(rare, 1, 4) > share(base, 1, 4) + 0.1,
        rare and ('%.3f vs %.3f'):format(share(rare, 1, 4), share(base, 1, 4)))
    drops.rarity_weights(nil)
    gear.wait(0.2)
    local back = drops.rarity_table()
    local restored = #back == #table0
    for i, e in ipairs(back) do restored = restored and e.weight == table0[i].weight end
    check('rarity weights: cleared, the table is as the game loaded it', restored)

    -- 3b. The two special rows: which items do they make? (new items near the creature: unique = rarity 15, set = the set flag)
    local function ground()
        local set = {}
        for _, it in ipairs(gear.world.items_near(cx, cy, 3000, {layer = clayer, limit = 100000})) do set[it.id] = it end
        return set
    end
    do
        local g, n, ex = ground(), 0, nil
        for _, it in pairs(g) do n = n + 1; ex = ex or it end
        print(('items near the creature now: %d (e.g. %s)'):format(n, ex and gear.json.encode(ex) or '-'))
    end
    local function census()
        local n, unique, setitems, by = 0, 0, 0, {}
        for _, it in pairs(ground()) do
            n = n + 1
            if it.rarity == 15 then unique = unique + 1 end
            if it.set then setitems = setitems + 1 end
            by[it.rarity] = (by[it.rarity] or 0) + 1
        end
        local parts = {}
        for r, c in pairs(by) do parts[#parts + 1] = ('r%d=%d'):format(r, c) end
        table.sort(parts)
        return n, unique, setitems, table.concat(parts, ' ')
    end
    print(('ground before the special rows: %d items, unique %d, set %d | %s'):format(census()))
    for _, key in ipairs({'special0', 'special1'}) do
        drops.rarity_weights({[key] = 1000})
        gear.wait(0.2)
        local known = ground()
        local s = run(creature, 300)
        describe(key .. ' x1000', s)
        print(('%s x1000 ground after: %d items, unique %d, set %d | %s'):format(key, census()))
        local fresh, f_unique, f_set, f_types = 0, 0, 0, {}
        for id, it in pairs(ground()) do
            if not known[id] then
                fresh = fresh + 1
                if it.rarity == 15 then f_unique = f_unique + 1 end
                if it.set then f_set = f_set + 1 end
                if #f_types < 8 then f_types[#f_types + 1] = ('%s/r%d%s'):format(tostring(it.type_name), it.rarity, it.set and '/set' or '') end
            end
        end
        print(('%s x1000 new items: %d, unique %d, set %d | %s'):format(key, fresh, f_unique, f_set, table.concat(f_types, ' ')))
        drops.rarity_weights(nil)
        gear.wait(0.2)
    end
    -- 4. Events.
    local veto = gear.on('drops.roll', function(ev) return false end)
    gear.wait(0.2)
    local vetoed = run(creature, 100)
    describe('veto', vetoed)
    check('drops.roll: a veto stops every roll', vetoed and vetoed.vetoed == 100 and vetoed.picks == 0, vetoed and vetoed.vetoed)
    gear.off(veto)

    local seen_event = 0
    local ev_sel
    local sel = gear.on('drops.select', function(ev)
        seen_event = seen_event + 1
        ev_sel = ev_sel or {category = ev.category, level = ev.level, quality = ev.quality}
        ev.type = tonumber(cfg.replace_type) or 0x13C1
    end)
    gear.wait(0.2)
    local sub = run(creature, N)
    describe('select replaced', sub)
    check('drops.select: the handler runs for every pick', sub and seen_event == sub.picks and sub.picks > 0, sub and (seen_event .. ' vs ' .. sub.picks))
    check('drops.select: every pick is replaced', sub and sub.overridden == sub.picks, sub and sub.overridden)
    print('first select event: ' .. gear.json.encode(ev_sel or {}))
    gear.off(sel)

    -- 5. Generators.
    drops.rng('fair')
    gear.wait(0.2)
    local fair = run(creature, N)
    describe('fair rng', fair)
    check('fair rng: the generator answers the draws', fair and fair.rand_calls > 0 and fair.lua_draws == 0, fair and fair.rand_calls)
    check('fair rng: drops still happen', fair and fair.picks > 10, fair and fair.picks)

    -- A seed makes the generator's numbers repeat (the drops themselves also depend on state the game keeps between rolls: the last
    -- source that dropped a rune or jewellery, the world around).
    drops.rng({seed = 12345})
    gear.wait(0.2)
    local a, b = {}, {}
    for i = 1, 20 do a[i] = drops.random(1000000) end
    drops.rng({seed = 12345})
    gear.wait(0.2)
    for i = 1, 20 do b[i] = drops.random(1000000) end
    drops.rng({seed = 777})
    gear.wait(0.2)
    local c = {}
    for i = 1, 20 do c[i] = drops.random(1000000) end
    check('seeded rng: the same seed repeats the generator\'s numbers', table.concat(a, ',') == table.concat(b, ','))
    check('seeded rng: another seed gives other numbers', table.concat(a, ',') ~= table.concat(c, ','))
    drops.rng({seed = 12345})
    gear.wait(0.2)
    local s1 = run(creature, N)
    describe('seeded run', s1)
    check('seeded rng: drops happen', s1 and s1.picks > 10 and s1.rand_calls > 0)

    local draws = 0
    drops.rng(function() draws = draws + 1; return 0.999 end)
    gear.wait(0.2)
    local lua_rng = run(creature, N)
    describe('lua rng 0.999', lua_rng)
    check('lua rng: the function answers the draws', lua_rng and lua_rng.lua_draws > 0 and draws == lua_rng.lua_draws, lua_rng and (lua_rng.lua_draws .. ' vs ' .. draws))
    check('lua rng: 0.999 fails every chance roll: no drops', lua_rng and lua_rng.picks == 0, lua_rng and lua_rng.picks)
    drops.rng(nil)
    gear.wait(0.2)
    local after = run(creature, N)
    describe('back to the game rng', after)
    check('back to the game generator', after and after.rand_calls == 0 and after.picks > 10)

    -- The fair generator itself: uniform in 0 .. 9999 (chi-square over 100 buckets, 200000 draws).
    local buckets, n = {}, 200000
    for i = 1, 100 do buckets[i] = 0 end
    for _ = 1, n do local v = drops.random(10000); local b = math.floor(v / 100) + 1; buckets[b] = buckets[b] + 1 end
    local expected, chi = n / 100, 0
    for i = 1, 100 do chi = chi + (buckets[i] - expected) ^ 2 / expected end
    check('fair generator: uniform (chi-square of 100 buckets below 135)', chi < 135, ('%.1f'):format(chi))
    finish()
end

gear.task(function()
    local ok, err = xpcall(body, debug and debug.traceback or tostring)
    if not ok then
        check('the test ran to the end', false, err)
        drops.rng(nil)
        drops.chance_scale(nil)
        drops.rarity_weights(nil)
        finish()
    end
end)
