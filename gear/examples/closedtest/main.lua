-- Drives Multiplayer > Closed Internet against a test lobby up to the world of a game server running on this machine (see
-- README.md in this folder). Clicks are in UI layout coordinates (1024x768 space, as the realm test); screenshots show each step.
--   login (account, password) -> character selection (create one if config.create) -> Lobby -> room #2 Closed Net -> the game list ->
--   Beitreten -> the host pane (game name, password, mode ...) -> Starten -> the world.
-- config.stop = 'games' stops at the game list (screenshots only). config.quit = false leaves the game running for another mod's test
-- (a result file is written by that mod).
local A = gear.auto
local cfg = gear.config

local function field(x, y, text, clear)
    A.ui_click(x, y) gear.wait(0.4)
    if clear then for _ = 1, clear do A.press('back') end gear.wait(0.2) end
    A.type(text) gear.wait(0.4)
end

gear.task(function()
    A.wait_for_menu(120)
    gear.wait(6)
    gear.realm.add({id = 'closedtest', name = 'Closed test', host = cfg.host, port = cfg.port})
    gear.realm.use('closedtest')
    A.ui_click(512, 352)            -- Multiplayer
    gear.wait(2)
    A.ui_click(512, 352)            -- Closed Internet (second entry)
    gear.wait(3)
    A.screenshot('ct-1-login')
    -- the account and password fields come filled with the guest login; the two CD key fields take any four characters
    field(512, 183, cfg.account, 24)
    field(512, 249, cfg.password, 24)
    field(430, 319, 'AAAA')
    field(430, 379, 'AAAA')
    A.screenshot('ct-2-filled')
    A.ui_click(512, 577)            -- Einloggen
    gear.wait(6)
    A.screenshot('ct-3-after-login')
    A.ui_click(508, 438) gear.wait(2)       -- Ok on the "invalid data block" message (a new account has no hero yet)
    gear.wait(4)
    A.screenshot('ct-4-select')
    if cfg.create then
        A.ui_click(94, 495) gear.wait(1.5)      -- an empty slot
        A.ui_click(508, 662) gear.wait(3)       -- Neuer Charakter
        if cfg.keep_class then A.ui_click(375, 392) gear.wait(1.5) else A.ui_click(102, 480) gear.wait(1.5) end     -- the Gladiator (the screen starts with the Dark Elf)
        field(512, 585, cfg.hero, 16)           -- the name
        gear.wait(0.5)
        A.ui_click(608, 713) gear.wait(4)       -- Ok
        A.screenshot('ct-5-created')
        if cfg.quit then A.quit(0) end
        return
    end
    A.ui_click(102, 480) gear.wait(2)           -- the hero on the first stone
    A.ui_click(580, 713) gear.wait(4)           -- Lobby
    A.screenshot('ct-6-lobby')
    A.ui_click(222, 97) gear.wait(1)            -- #2 Closed Net
    A.ui_click(469, 382) gear.wait(4)           -- Betreten
    A.screenshot('ct-7-games')
    if cfg.stop == 'games' then
        if cfg.quit then A.quit(0) end
        return
    end
    A.ui_click(222, 65) gear.wait(1)            -- the game
    A.ui_click(469, 382) gear.wait(6)           -- Beitreten: the first player of a server is its host and gets the game pane
    A.screenshot('ct-8-host-pane')
    -- The pane's Starten does nothing until the game's password is typed (the server's NETWORK_PASSWORD, from Gameserver.cfg).
    field(401, 160, cfg.game_password)
    A.ui_click(469, 382) gear.wait(5)           -- Starten
    local ok = A.wait_for_game(180)
    gear.wait(5)
    A.screenshot('ct-9-ingame')
    if cfg.world_actions then
        -- walk to the merchant left of the start (pixels of the 1600x900 window) and open his trade window
        A.click(176, 610) gear.wait(10)
        A.screenshot('ct-10-merchant')
        A.click(176, 610) gear.wait(3)
        A.screenshot('ct-11-merchant2')
        if cfg.buy then
            -- Buy something: try the cells of the merchant's ware grid (pixels) until the hero's gold goes down. A ware is picked up on the
            -- cursor with a click and paid for when it is put into the backpack.
            local hero = gear.world.hero()
            local gold0 = hero and hero:gold() or 0
            local bought = false
            for y = 86, 330, 32 do
                for x = 16, 480, 32 do
                    A.click(x, y, cfg.buy_button) gear.wait(0.5)
                    if not cfg.buy_button then A.click(220 + (x % 64), 520) gear.wait(0.7) end
                    if hero and hero:gold() < gold0 then bought = true break end
                end
                if bought then break end
            end
            A.screenshot('ct-13-bought')
            gear.log(('closedtest: purchase %s (gold %d -> %d)'):format(bought and 'done' or 'not done', gold0, hero and hero:gold() or -1))
            gear.wait(8)
        end
    end
    if cfg.inspect then
        -- what is around the hero: positions and kinds (to aim clicks at world containers and monsters deterministically)
        local hero = gear.world.hero()
        local hx, hy = hero:position()
        local out = {hero = {x = hx, y = hy}, near = {}, items = {}}
        for _, c in ipairs(gear.world.creatures_near(hx, hy, 3000, {limit = 300})) do
            local x, y = c:position()
            out.near[#out.near + 1] = {t = c:type_name(), id = c.id, x = x, y = y, hp = c:hp(), name = c:name(), dead = c:is_dead() or nil}
        end
        for _, it in ipairs(gear.world.items_near(hx, hy, 3000, {limit = 100})) do
            out.items[#out.items + 1] = {t = it.type_name, x = it.x, y = it.y}
        end
        A.write('inspect.json', gear.json.encode(out))
        A.screenshot('ct-15-inspect')
    end
    if cfg.portal then
        -- the transfer portal next to the start (it takes the hero to the mainland): walk into it, then look around
        A.click(1040, 340) gear.wait(8)
        A.screenshot('ct-19-portal1')
        A.click(1040, 340) gear.wait(8)
        A.screenshot('ct-19-portal2')
    end
    if cfg.walk_to then
        -- walk towards a world position in steps (pixel mapping as for fight), logging progress and what is near on arrival
        local hero = gear.world.hero()
        local tx, ty = cfg.walk_to[1], cfg.walk_to[2]
        local log = {}
        for i = 1, cfg.walk_steps or 80 do
            local hx, hy = hero:position()
            local dx, dy = tx - hx, ty - hy
            local d = math.sqrt(dx * dx + dy * dy)
            if d < 400 then break end
            local k = math.min(1, 450 / d)
            local sx = 800 + 0.89 * ((dx * k) - (dy * k))
            local sy = 444 + 0.42 * ((dx * k) + (dy * k))
            A.click(sx, sy) gear.wait(2.5)
            if i % 5 == 0 then log[#log + 1] = ('%d: %d,%d d=%d'):format(i, hx, hy, d) end
        end
        local hx, hy = hero:position()
        log[#log + 1] = ('end %d,%d'):format(hx, hy)
        A.write('walk.txt', table.concat(log, ';'))
        A.screenshot('ct-18-arrived')
    end
    if cfg.fight then
        -- deterministic aiming: the screen is isometric around the hero (calibrated: pixel = hero pixel + (0.89 * (dx - dy), 0.42 * (dx + dy)))
        local hero = gear.world.hero()
        local function px(wx, wy)
            local hx, hy = hero:position()
            return 800 + 0.89 * ((wx - hx) - (wy - hy)), 444 + 0.42 * ((wx - hx) + (wy - hy))
        end
        local log = {}
        local target
        local sx0, sy0 = hero:position()
        for _, c in ipairs(gear.world.creatures_near(sx0, sy0, 2500, {limit = 200})) do
            if c:type_name() == 'gob_warrior.grn' then target = c break end
        end
        log[#log + 1] = 'target ' .. tostring(target and target.id)
        for i = 1, 60 do
            if not target or target:is_dead() then break end
            local x, y = target:position()
            local sx, sy = px(x, y)
            A.click(sx, sy) gear.wait(1)
        end
        log[#log + 1] = 'target dead ' .. tostring(target and target:is_dead()) .. ' hero hp ' .. tostring(hero:hp())
        gear.wait(4)
        A.screenshot('ct-16-after-fight')
        local hx, hy = hero:position()
        for _, it in ipairs(gear.world.items_near(hx, hy, 1500, {limit = 30})) do
            log[#log + 1] = 'item ' .. tostring(it.type_name) .. ' ' .. (it.x - hx) .. ',' .. (it.y - hy)
            if it.type_name ~= 'wegweiser_01.grn' then
                local sx, sy = px(it.x, it.y)
                A.click(sx, sy) gear.wait(4)
            end
        end
        A.screenshot('ct-17-after-pickup')
        A.write('fight.txt', table.concat(log, ';'))
    end
    if cfg.containers then
        -- the chest and the barrels around the start (pixels of the 1600x900 window): walk there, open them
        for i, spot in ipairs({{980, 430}, {690, 580}, {740, 750}, {930, 660}}) do
            A.click(spot[1], spot[2]) gear.wait(7)
            A.click(spot[1], spot[2]) gear.wait(3)
            A.screenshot('ct-14-container' .. i)
        end
    end
    local bp = gear.inv.container('backpack')
    A.write('closedtest.json', gear.json.encode({in_game = ok, mode = gear.mp.mode(), gold = gear.world.hero() and gear.world.hero():gold() or -1, backpack = bp and #bp:items() or -1, types = (function() local l = {} for _, it in ipairs(bp and bp:items() or {}) do l[#l + 1] = tostring(it.type_name) .. '#' .. it.id end return table.concat(l, ',') end)()}))
    local count = bp and #bp:items() or -1
    local good = ok and (cfg.expect_backpack == nil or cfg.expect_backpack == count)
    if cfg.expect_backpack ~= nil and not good then gear.log(('closedtest: %d items in the backpack, expected %d'):format(count, cfg.expect_backpack)) end
    if cfg.quit then A.quit(good and 0 or 1) end
end)
