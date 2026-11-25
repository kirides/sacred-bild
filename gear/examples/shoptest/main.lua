-- Self test of `gear.shop` and the `shop.stock` event, in a loaded savegame (-gear-auto -gear-load=N): teleports to a merchant of the world map, finds the one
-- whose click opens the shop window, records the stock, closes and reopens it several times and compares. Without an engine restock event in between the
-- stock must be identical. Results go to the log ("shoptest: ...") and to result.json.
local A = gear.auto

local events, results = {}, {}
gear.on('shop.stock', function(e)
    events[#events + 1] = e.reason
    gear.log(('shoptest: shop.stock %s merchant %d container %d level %d'):format(e.reason, e.merchant, e.container, e.hero_level))
end)

local function snapshot()
    local out = {}
    for _, it in ipairs(gear.shop.items()) do
        out[#out + 1] = ('%d:%s q=%s'):format(it.id, it.type_name or it.type, tostring(it.quality))
    end
    table.sort(out)
    return out
end

local function check(name, ok, detail)
    results[#results + 1] = {name = name, ok = ok and true or false, detail = detail}
    gear.log(('shoptest: %s %s %s'):format(ok and 'PASS' or 'FAIL', name, detail or ''))
end

local function shop_open()
    local info = gear.shop.info()
    return info and info.open
end

-- screen position (1600x900) of a world position relative to the hero
local function screen(hero, x, y)
    local hx, hy = hero:position()
    local dx, dy = x - hx, y - hy
    return 800 + 0.89 * (dx - dy), 440 + 0.42 * (dx + dy)
end

local function open_on(hero, c)
    local x, y = c:position()
    local sx, sy = screen(hero, x, y)
    if sx < 20 or sx > 1580 or sy < 20 or sy > 780 then return false end
    A.click(sx, sy - 30)
    for _ = 1, 16 do
        gear.wait(0.5)
        if shop_open() then return true end
    end
    return false
end

gear.task(function()
    A.wait_for_game(180)
    local hero = gear.world.hero()
    local hx, hy = hero:position()
    local markers = {}
    for _, m in ipairs(gear.game.map_markers() or {}) do
        if m.kind == 0 then
            markers[#markers + 1] = {m = m, d = (m.x * gear.world.TILE - hx) ^ 2 + (m.y * gear.world.TILE - hy) ^ 2}
        end
    end
    table.sort(markers, function(p, q) return p.d < q.d end)
    local skip = {'RABBIT', 'WOLF', 'CHICKEN', 'PIG', 'DEER', 'CHILD', 'DARKELVENPC', 'ELVE_SORC', 'BLACK_MAGICIAN', 'SLAVE', 'THIEF', 'COW', 'SHEEP', 'DOG', 'CAT'}
    local merchant, tries = nil, 0
    for mi = 1, math.min(#markers, 8) do
        if merchant then break end
        local m = markers[mi].m
        hero:teleport(m.x * gear.world.TILE, m.y * gear.world.TILE)
        gear.wait(4)
        hx, hy = hero:position()
        gear.log(('shoptest: marker %d at %d,%d tile %d,%d'):format(mi, hx, hy, m.x, m.y))
        for _, c in ipairs(gear.world.creatures_near(hx, hy, 700, {limit = 60})) do
            local t = (c:type_name() or ''):upper()
            local skipped = c.id == hero.id
            for _, w in ipairs(skip) do skipped = skipped or t:find(w, 1, true) ~= nil end
            if not skipped and tries < 40 then
                tries = tries + 1
                local cx, cy = c:position()
                hero:teleport(cx - 150, cy - 150)
                gear.wait(1)
                cx, cy = c:position()
                gear.log(('shoptest: try %d %s at %d,%d'):format(c.id, t, cx, cy))
                if open_on(hero, c) then merchant = c break end
            end
        end
    end
    A.screenshot('shoptest-1')
    check('a merchant opened', merchant ~= nil)
    if not merchant then A.write('result.json', gear.json.encode(results)) A.quit(1) return end
    local info = gear.shop.info()
    gear.log(('shoptest: merchant %d pages %s'):format(info.merchant, gear.json.encode(info.pages)))
    local first = snapshot()
    gear.log(('shoptest: first stock %d wares: %s'):format(#first, table.concat(first, ', ')))
    check('first stock logged a new event', events[1] == 'new' or events[1] == 'refresh', tostring(events[1]))
    check('shop has wares', #first > 0)
    -- the API: add by category (level and quality asked for), add by type, remove
    local lvl = hero:level()
    check('event hero level matches the hero', info.hero_level == lvl, ('%s vs %s'):format(tostring(info.hero_level), tostring(lvl)))
    local page = info.pages[1]
    local cat = page.categories[1].category
    local added = {}
    for i = 1, 3 do
        local id, err = gear.shop.add{container = page.container, category = cat, quality = 14, level = 40}
        check('add by category ' .. i, id ~= nil, tostring(err))
        if id then
            added[#added + 1] = id
            local it = gear.world.item(id)
            gear.log(('shoptest: added %d %s quality %s level %s price %s'):format(id, it and it.type_name or '?', it and tostring(it.quality) or '?', it and tostring(it.level) or '?', it and tostring(it.price) or '?'))
        end
    end
    local vals = added[1] and gear.inv.values(added[1])
    check('added item has the asked level', vals and vals.level == 40, vals and tostring(vals.level) or 'no values')
    local picked = gear.shop.pick{category = cat, level = 40, quality = 14}
    check('pick gives a type', picked ~= nil, tostring(picked))
    local id2, err2 = gear.shop.add{container = page.container, type = picked, quality = 0}
    check('add by type', id2 ~= nil, tostring(err2))
    local now = #gear.shop.items()
    check('items grew', now == #first + #added + (id2 and 1 or 0), ('%d vs %d'):format(now, #first + #added + (id2 and 1 or 0)))
    local gone = id2 and gear.shop.remove(id2)
    check('remove', gone == true)
    check('items back', #gear.shop.items() == #first + #added, tostring(#gear.shop.items()))
    local keep = added[1]
    local prev = snapshot()
    for round = 1, 6 do
        A.press('escape') gear.wait(1.5)
        local n = #events
        local ok = open_on(hero, merchant)
        check('reopen ' .. round, ok)
        if ok then
            local now = snapshot()
            local restocked = #events > n
            local same = table.concat(now, ',') == table.concat(prev, ',')
            gear.log(('shoptest: reopen %d: %d wares, restock event %s, same %s'):format(round, #now, tostring(restocked and events[#events]), tostring(same)))
            if not restocked then check('reopen ' .. round .. ' without restock shows the same stock', same) end
            local still = false
            for _, it in ipairs(gear.shop.items()) do still = still or it.id == keep end
            gear.log(('shoptest: added ware %d still there: %s'):format(keep or 0, tostring(still)))
            if not restocked then check('reopen ' .. round .. ' keeps the added ware', still) end
            prev = now
        end
    end
    A.screenshot('shoptest-2')
    A.write('result.json', gear.json.encode({results = results, events = events}))
    A.quit(0)
end)
