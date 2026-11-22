-- Test of gear.pfx (Sacred.exe -gear-auto -gear-load=N -gear-mods=pfxtest). The hero casts spells whose effects are known (5, 6, 12, 30);
-- first as the engine does, then with variants: a colour change by a rule, a level condition, a function rule with half speed, and a
-- texture swap. Screenshots go to GearAutomation\<instance>.
if not gear.auto then return end
local A = gear.auto
local res = {passed = 0, failed = 0}
local function check(n, ok, d)
    if ok then res.passed = res.passed + 1 else res.failed = res.failed + 1 end
    print((ok and 'PASS ' or 'FAIL ') .. n .. (d ~= nil and (': ' .. tostring(d)) or ''))
end

local seen = {}
local function note(ctx)
    local key = ('fx=0x%X creature=%s ability=%s level=%s parent=%s'):format(ctx.fx, tostring(ctx.creature and ctx.creature.id),
        tostring(ctx.ability), tostring(ctx.level), tostring(ctx.parent))
    if not seen[key] then seen[key] = true print('PFX ' .. key) end
end

local function cast(h, id, wait_after)
    local x, y = h:position()
    for _ = 1, 40 do
        local a = h:ability(id)
        if a and a.ready then break end
        gear.wait(0.5)
    end
    local ok, err = h:order_ability(id, x + 250, y)
    gear.wait(wait_after or 1.2)
    return ok, err
end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then A.quit(1) return end
    gear.wait(3)
    local h = gear.world.hero()
    local spells = {5, 6}
    for _, id in ipairs(spells) do
        if not h:ability(id) then h:learn(id, 5) end
    end
    gear.wait(6)

    -- 1. what the engine does, and what the rules see
    local handler = gear.on('pfx.select', note)
    local shots = {0.6, 1.4, 2.4}
    local function shoot(tag, id)
        cast(h, id, 0)
        local t = 0
        for i, at in ipairs(shots) do
            gear.wait(at - t)
            t = at
            A.screenshot(('%s-%d-%d'):format(tag, id, i))
        end
        gear.wait(9)
    end
    for _, id in ipairs(spells) do
        print('PFX baseline cast ' .. id)
        shoot('base', id)
    end
    gear.off(handler)
    local types = {}
    for k in pairs(seen) do types[#types + 1] = tonumber(k:match('fx=0x(%x+)'), 16) end
    table.sort(types)
    print('PFX types', table.concat(types, ','))
    check('effects seen', #types > 0, #types)
    local with_caster = false
    for k in pairs(seen) do if k:find('creature=1 ', 1, true) then with_caster = true end end
    check('rules see the caster', with_caster)
    check('no variant applied without definitions', gear.pfx.stats().applied == 0, gear.pfx.stats().applied)

    -- 2. "if the skill level is 5 or more, red instead of green": a colour rule for every effect type of those spells
    gear.pfx.define({id = 1, colorize = 0xFF2020, name = 'red'})
    check('definitions', #gear.pfx.definitions() == 1)
    local rule = gear.pfx.rule({fx = types, min_level = 5, use = 1})
    for _, id in ipairs(spells) do
        print('PFX red cast ' .. id)
        shoot('red', id)
    end
    check('variants applied', gear.pfx.stats().applied > 0, gear.pfx.stats().applied)
    gear.pfx.remove_rule(rule)

    -- 3. a level condition: the spells are level 5
    local a0 = gear.pfx.stats().applied
    local r2 = gear.pfx.rule({fx = types, min_level = 6, use = 1})
    cast(h, 5, 3)
    check('min_level 6 does not apply at level 5', gear.pfx.stats().applied == a0, gear.pfx.stats().applied - a0)
    gear.pfx.remove_rule(r2)
    gear.wait(9)
    local r3 = gear.pfx.rule({fx = types, min_level = 5, max_level = 5, use = 1})
    cast(h, 5, 3)
    check('min_level 5 applies at level 5', gear.pfx.stats().applied > a0, gear.pfx.stats().applied - a0)
    gear.pfx.remove_rule(r3)
    gear.wait(9)

    -- 4. a function rule with a hue shift and half speed
    gear.pfx.define({id = 2, speed = 0.4, hue = 120})
    local seen_ctx
    local r4 = gear.pfx.rule({fx = types, select = function(ctx) seen_ctx = seen_ctx or ctx return 2 end})
    shoot('slow', 5)
    check('select got the casting creature', seen_ctx and seen_ctx.creature and seen_ctx.creature.id == h.id, seen_ctx and tostring(seen_ctx.creature))
    gear.pfx.remove_rule(r4)

    -- 5. another texture: the glow sprites of the hand effect
    local swap = {}
    for _, n in ipairs({'GLOW01', 'GLOW03', 'FLARE02', 'FLARE04', 'SPARK04', 'SPARK07', 'FIRE01', 'FIRE02', 'FIRE03', 'FIRE04', 'FIRE05', 'FIRE06'}) do
        swap['PARTICLE_' .. n .. '.TGA'] = 'PARTICLE_SMOKE01.TGA'
    end
    gear.pfx.define({id = 3, textures = swap})
    local r5 = gear.pfx.rule({fx = types, use = 3})
    shoot('tex', 5)
    gear.pfx.remove_rule(r5)

    print(('PFX result %d passed %d failed'):format(res.passed, res.failed))
    A.write('pfx.json', gear.json.encode(res))
    A.quit(res.failed == 0 and 0 or 1)
end)
