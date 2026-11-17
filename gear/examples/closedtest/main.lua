-- Drives Multiplayer > Closed Internet against a test lobby (see tools/closed-net-test.md). Steps are screenshots + clicks in UI layout
-- coordinates (1024x768 space, as the realm test).
local A = gear.auto
local cfg = gear.config
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
    local fields = {{512, 183, cfg.account}, {512, 249, cfg.password}, {430, 319, 'AAAA'}, {430, 379, 'AAAA'}}
    for i, f in ipairs(fields) do
        A.ui_click(f[1], f[2]) gear.wait(0.4)
        if i <= 2 then for _ = 1, 24 do A.press('back') end gear.wait(0.2) end     -- the fields come filled with the guest login
        A.type(f[3]) gear.wait(0.4)
    end
    A.screenshot('ct-2-filled')
    A.ui_click(512, 577)            -- Einloggen
    gear.wait(6)
    A.screenshot('ct-3-after-login')
    A.ui_click(508, 438) gear.wait(2)       -- Ok on the "invalid data block" message
    gear.wait(4)
    A.screenshot('ct-4-select')
    if cfg.create then
        A.ui_click(94, 495) gear.wait(1.5)      -- an empty slot
        A.ui_click(508, 662) gear.wait(3)       -- Neuer Charakter
        A.ui_click(102, 480) gear.wait(1.5)     -- the Gladiator
        A.ui_click(512, 585) gear.wait(0.5)     -- the name
        for _ = 1, 16 do A.press('back') end
        A.type('Stasha')
        gear.wait(0.5)
        A.ui_click(608, 713) gear.wait(4)       -- Ok
        A.screenshot('ct-6-created')
    end
    if not cfg.create then
        A.ui_click(102, 480) gear.wait(2)       -- the hero
        A.screenshot('ct-5-hero')
        A.ui_click(580, 713) gear.wait(4)       -- Lobby
        A.screenshot('ct-6-lobby')
        A.ui_click(222, 97) gear.wait(1)        -- #2 Closed Net
        A.ui_click(469, 382) gear.wait(4)       -- Betreten
        A.screenshot('ct-7-games')
        A.ui_click(222, 65) gear.wait(1)        -- the game
        A.ui_click(469, 382) gear.wait(3)       -- Beitreten
        A.screenshot('ct-8-joining')
        gear.wait(3)
        A.press('enter') gear.wait(3)
        A.screenshot('ct-8a-enter')
        for _, x in ipairs({438, 500, 469}) do A.ui_click(x, 382) gear.wait(2) A.screenshot('ct-8b-click' .. x) end
        A.screenshot('ct-8b-started')
        local ok = A.wait_for_game(120)
        gear.wait(10)
        A.screenshot('ct-9-ingame')
        local out = {in_game = ok, mode = gear.mp.mode(), network = gear.world and gear.mp.info and gear.mp.info() or nil}
        A.write('closedtest.json', gear.json.encode(out))
    end
    if cfg.quit then A.quit(0) end
end)
