-- Drives the menus to host a LAN game; screenshots after every step (GearAutomation\<instance>).
-- Run: Sacred.exe -gear-auto -gear-mods=lanhosttest[,other] -gear-instance=NAME
local A = gear.auto
gear.task(function()
    A.wait_for_menu(120)
    gear.wait(8)
    A.screenshot('lan-0-menu')
    A.ui_click(512, 352)       -- Multiplayer
    gear.wait(2)
    A.screenshot('lan-1-multiplayer')
    A.ui_click(512, 352 - 38 / 1.1719)   -- LAN
    gear.wait(3)
    A.screenshot('lan-2-lan')
    -- px (1600x900 shot, UI area x 200..1400) -> UI layout (1024x768)
    local function px(x, y) return (x - 200) / 1.1719, y / 1.1719 end
    A.ui_click(px(gear.config.hero_x, gear.config.hero_y))     -- the hero (default: the second one)
    gear.wait(2)
    A.ui_click(px(910, 838))     -- Weiter
    gear.wait(3)
    A.screenshot('lan-3-after-hero')
    A.ui_click(px(553, 448))     -- Erstellen
    gear.wait(3)
    A.screenshot('lan-4-create')
    A.ui_click(px(747, 448))     -- Erstellen: the game starts its gameserver and connects to it
    for i = 1, 4 do
        gear.wait(4)
        A.screenshot('lan-5-hosting-' .. i)
    end
    A.write('lanhosttest.json', gear.json.encode({ui = A.ui(), balance_served = gear.files and gear.files.served and gear.files.served('bin' .. string.char(92) .. 'balance.bin')}))
    A.quit(0)
end)
