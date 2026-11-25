-- Loads savegame config.slot, waits for the world, and writes the hero's position and a census of the surroundings.
local A = gear.auto
gear.task(function()
    A.wait_for_menu(120)
    A.load_save(gear.config.slot)
    if not A.wait_for_game(180) then A.write('saveinfo.json', '{"in_game":false}') A.quit(1) return end
    gear.wait(8)
    local hero = gear.world.hero()
    local hx, hy = hero:position()
    local out = {slot = gear.config.slot, hero = {x = hx, y = hy, class = hero:class(), level = hero:level()}, kinds = {}, items = {}}
    for _, c in ipairs(gear.world.creatures_near(hx, hy, gear.config.radius, {limit = 400})) do
        local k = tostring(c:type_name())
        local e = out.kinds[k] or {n = 0, hp = c:hp(), nearest = nil}
        e.n = e.n + 1
        local x, y = c:position()
        local d = math.floor(math.sqrt((x - hx) ^ 2 + (y - hy) ^ 2))
        if not e.nearest or d < e.nearest then e.nearest = d end
        out.kinds[k] = e
    end
    for _, it in ipairs(gear.world.items_near(hx, hy, gear.config.radius, {limit = 100})) do
        local d = math.floor(math.sqrt((it.x - hx) ^ 2 + (it.y - hy) ^ 2))
        out.items[#out.items + 1] = {t = it.type_name, d = d}
    end
    A.write('saveinfo.json', gear.json.encode(out))
    A.screenshot('saveinfo')
    A.quit(0)
end)
