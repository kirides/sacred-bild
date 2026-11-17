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
        A.ui_click(102, 480) gear.wait(1.5)     -- the Gladiator
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
    local bp = gear.inv.container('backpack')
    A.write('closedtest.json', gear.json.encode({in_game = ok, mode = gear.mp.mode(), gold = gear.world.hero() and gear.world.hero():gold() or -1, backpack = bp and #bp:items() or -1, types = (function() local l = {} for _, it in ipairs(bp and bp:items() or {}) do l[#l + 1] = tostring(it.type_name) .. '#' .. it.id end return table.concat(l, ',') end)()}))
    local count = bp and #bp:items() or -1
    local good = ok and (cfg.expect_backpack == nil or cfg.expect_backpack == count)
    if cfg.expect_backpack ~= nil and not good then gear.log(('closedtest: %d items in the backpack, expected %d'):format(count, cfg.expect_backpack)) end
    if cfg.quit then A.quit(good and 0 or 1) end
end)
