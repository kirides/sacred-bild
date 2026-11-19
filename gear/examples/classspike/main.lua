if not gear.auto then return end
local A = gear.auto
local party = gear.party
local log = {}
local function say(fmt, ...)
    local line = select('#', ...) > 0 and fmt:format(...) or fmt
    log[#log + 1] = line
    print('SPIKE ' .. line)
end

local route_on, route_map = false, {}
local seen_routes, seen_slots, slot_map = {}, {}, {}
gear.on('ability.route', function(t, art, vanilla, action, id)
    seen_routes[#seen_routes + 1] = ('%d/%d/%d/%d'):format(art, vanilla, action, t)
    if route_on and route_map[art] then return route_map[art] end
end)
gear.on('motion.select', function(t, slot, id)
    seen_slots[#seen_slots + 1] = slot
    if slot_map[slot] then return slot_map[slot] end
end)

local function foe_for(hero)
    local x, y, layer = hero:position()
    for _, c in ipairs(gear.world.creatures_near(x, y, 3500, {layer = layer})) do
        if not c:is_dead() and hero:is_enemy(c) and not c:is_hero() then return c end
    end
end

local function try_art(comp, art, label, opts)
    opts = opts or {}
    seen_routes, seen_slots = {}, {}
    local foe = foe_for(comp)
    if not foe then say('%s: no foe', label) return end
    local x, y = comp:position()
    foe:teleport(x + 130, y - 130)
    gear.wait(0.4)
    local hp0 = foe:hp()
    local ok, why = comp:order_ability(art, foe)
    local maxregen, trace, last = 0, {}, nil
    for i = 1, 26 do
        gear.wait(0.15)
        local a = comp:ability(art)
        if a and a.regen > maxregen then maxregen = a.regen end
        local act = comp:action()
        if act ~= last then trace[#trace + 1] = ('%.1f:%s'):format(i * 0.15, tostring(act)); last = act end
        if i == 6 and opts.shot then A.screenshot(opts.shot) end
    end
    local hp1 = foe:is_dead() and 0 or foe:hp()
    say('%s: order=%s%s used(maxregen)=%.1f hp %s -> %s, actions[%s], routes#%d first[%s], slots[%s]', label, tostring(ok), why and (' ' .. tostring(why)) or '',
        maxregen, tostring(hp0), tostring(hp1), table.concat(trace, ' '), #seen_routes, tostring(seen_routes[1]), table.concat(seen_slots, ','))
    comp:order_move(x, y)
    gear.wait(0.8)
end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then A.quit(1) return end
    gear.wait(3)
    local main = party.leader()
    local ser = party.recruit('seraphim', {experience = 120000})
    say('recruited seraphim=%s', tostring(ser and ser.id))
    if not ser then A.quit(1) return end
    gear.wait(1)

    -- S1: Seraphim casts Battle Mage spells
    for _, s in ipairs({5, 8, 34}) do
        say('learn %d: %s', s, tostring(select(2, ser:learn(s, 1)) or 'ok'))
    end
    try_art(ser, 5, 'S1 seraphim casts fireball (5)', {shot = 'S1-fireball'})
    try_art(ser, 8, 'S1 seraphim casts ball lightning (8)')
    try_art(ser, 34, 'S1 seraphim casts ice shards (34)')

    -- S2: Seraphim uses Gladiator moves, without routing, then with
    local g_arts = {1057, 1058, 1059, 1060}
    for _, a in ipairs(g_arts) do say('learn %d: %s', a, tostring(select(2, ser:learn(a, 1)) or 'ok')) end
    route_on = false
    for _, a in ipairs(g_arts) do try_art(ser, a, 'S2 seraphim gladiator art ' .. a .. ' NO routing', {shot = a == 1058 and 'S2-noroute' or nil}) end
    route_on = true
    for _, a in ipairs(g_arts) do route_map[a] = 2 end
    for _, a in ipairs(g_arts) do try_art(ser, a, 'S2 seraphim gladiator art ' .. a .. ' ROUTED', {shot = a == 1058 and 'S2-route' or nil}) end
    route_on = false

    -- S3: Gladiator casts a spell: no clip / foreign clip / remapped own clip
    party.dismiss(ser)
    gear.wait(1)
    local gla = party.recruit('gladiator', {experience = 120000})
    say('recruited gladiator=%s', tostring(gla and gla.id))
    if not gla then A.quit(1) return end
    gear.wait(1)
    say('learn gl 5: %s', tostring(select(2, gla:learn(5, 1)) or 'ok'))
    say('gladiator has_motion(155)=%s seraphim=%s', tostring(gear.models.has_motion(2, 155)), tostring(gear.models.has_motion(1, 155)))
    try_art(gla, 5, 'S3 gladiator fireball, no clip', {shot = 'S3-noclip'})
    gear.models.set_motion(2, 155, gear.models.motion(1, 155))
    say('gladiator slot155 -> %s', tostring(gear.models.motion(2, 155)))
    try_art(gla, 5, 'S3 gladiator fireball, FOREIGN clip (seraphim 155)', {shot = 'S3-foreign'})
    gear.models.restore()
    slot_map[155] = 146
    try_art(gla, 5, 'S3 gladiator fireball, remapped to own special 146', {shot = 'S3-own'})
    slot_map = {}

    A.write('spike.json', gear.json.encode({log = log}))
    A.quit(0)
end)
