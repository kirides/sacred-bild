-- Casts every art and spell (ids from gear.data.abilities) with a hero of a class that may use it and records the effect types each makes
-- (the pfx.select handler sees every new effect: its type, the caster, the ability in flight and the cast level).
-- Output: GearAutomation\<instance>\pfx_table.json. Needs a single player savegame with a hero and a creature or two nearby; run it with
-- -gear-timeout=0 (it takes about half an hour): -gear-auto -gear-load=N -gear-mods=pfxtable -gear-timeout=0.
if not gear.auto then return end
local A = gear.auto
local table_ = {}      -- [id] = {owned = {fx = count}, loose = {fx = count}, target, caster}
local cur
local casters = {}     -- creature id -> class name
local function rec(id)
    local r = table_[id]
    if not r then r = {owned = {}, loose = {}} table_[id] = r end
    return r
end
gear.on('pfx.select', function(ctx)
    if not cur then return end
    if ctx.ability and ctx.creature and casters[ctx.creature.id] then
        local r = rec(ctx.ability)
        r.owned[ctx.fx] = (r.owned[ctx.fx] or 0) + 1
    else
        local r = rec(cur)
        r.loose[ctx.fx] = (r.loose[ctx.fx] or 0) + 1
    end
end)

local function has(t) return next(t) ~= nil end
local function write(done)
    local out = {}
    for id, r in pairs(table_) do
        local o, l = {}, {}
        for fx in pairs(r.owned) do o[#o + 1] = fx end
        for fx in pairs(r.loose) do l[#l + 1] = fx end
        table.sort(o) table.sort(l)
        out[#out + 1] = {id = id, art = id >= 1000, owned = o, loose = l, target = r.target, target_mode = r.mode, caster = r.caster, tried = r.tried}
    end
    table.sort(out, function(a, b) return a.id < b.id end)
    A.write('pfx_table.json', gear.json.encode({done = done, abilities = out}))
end

local function flat(name) return (name:gsub('_', '')) end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then A.quit(1) return end
    gear.wait(3)
    local hero = gear.world.hero()
    local bits = gear.classes.ability_classes
    -- one caster per class: the hero's own and a recruited companion for each other class
    local list = {}
    local own = flat(hero:class())
    casters[hero.id] = own
    for name, bit in pairs(bits) do
        if flat(name) == own then
            list[#list + 1] = {name = name, bit = bit, creature = hero}
        else
            local ok, c = pcall(gear.party.recruit, flat(name), {experience = 500000})
            if ok and c then
                casters[c.id] = name
                list[#list + 1] = {name = name, bit = bit, creature = c}
                print('PFXT caster', name, c.id)
            else
                print('PFXT no caster for', name, ok and 'nil' or c)
            end
            gear.wait(1)
        end
    end
    print('PFXT own class', own, hero.id, #list)
    local ids = gear.data.abilities:ids()
    -- no regeneration, so every cast can follow at once (the live table in this run only)
    local edits = {}
    for _, id in ipairs(ids) do edits[id] = {regen_base = 0, regen_per_level = 0} end
    local n, problems = gear.data.abilities:apply(edits)
    print('PFXT regen cleared', n, problems and #problems)
    gear.wait(2)
    local count = 0
    for _, id in ipairs(ids) do
        cur = id
        local r = rec(id)
        r.tried = {}
        local ab = gear.data.abilities[id]
        r.mode = ab and ab.target_mode
        local mask = ab and ab.class_mask or 0
        local choice = {}
        for _, c in ipairs(list) do if mask & c.bit ~= 0 then choice[#choice + 1] = c end end
        if #choice == 0 then choice[1] = list[1] end
        for _, c in ipairs(choice) do
            local h = c.creature
            if h:is_dead() then h:revive(h:max_hp()) end
            h:set_hp(h:max_hp())
            if not h:ability(id) then h:learn(id, 5) end
            local x, y = h:position()
            local other
            for _, o in ipairs(gear.world.creatures_near(x, y, 1500)) do
                if not casters[o.id] then other = o break end
            end
            local attempts = {
                {'ground', function() return h:order_ability(id, x + 250, y) end},
                {'self', function() return h:order_ability(id, h) end},
            }
            if other then attempts[#attempts + 1] = {'creature', function() return h:order_ability(id, other) end} end
            for _, at in ipairs(attempts) do
                at[2]()
                r.tried[#r.tried + 1] = c.name .. ':' .. at[1]
                gear.wait(2.6)
                if has(r.owned) or has(r.loose) then r.target = at[1] r.caster = c.name break end
            end
            if r.caster then break end
        end
        count = count + 1
        if count % 15 == 0 then print('PFXT progress', count, #ids) write(false) end
    end
    cur = nil
    write(true)
    print('PFXT done', count)
    A.quit(0)
end)
