-- Draws a small overlay with gear.ui: a translucent panel with the hero's values, the game's own images in the three
-- fit modes, a note that follows the inventory window, and a line above everything on the top layer.
--
-- As a self test (no player needed):
--
--     Sacred.exe -gear-auto -gear-load=1 -gear-mods=uidemo
--
-- loads savegame 1, takes screenshots (GearAutomation\uidemo-*.bmp) with the HUD, the inventory and the escape menu
-- open, writes GearAutomation\uidemo.json and quits.

local cfg = gear.config
local ui = gear.ui

local PANEL = ui.rgba(0, 0, 0, 150)
local EDGE = ui.rgba(200, 170, 90, 220)
local GOLD = ui.rgba(255, 215, 120)

-- Two images of the game's UI sheets: a combat art icon (square) and a portrait (taller than wide).
local icon, portrait
gear.on('load', function()
    icon = (ui.images('GUI_spell01')[1] or {}).id
    portrait = (ui.images('GUI_portraitX01')[1] or {}).id
    print(('images: icon %s, portrait %s'):format(tostring(icon), tostring(portrait)))
end)

local function panel(hero)
    local x, y, w, h = 16, 16, 260, 92
    ui.rect(x, y, w, h, PANEL)
    ui.frame(x, y, w, h, EDGE, 1)
    ui.text('Sacred Gear', x + 8, y + 6, {color = GOLD})
    if hero then
        ui.text(('Level %d'):format(hero:level() or 0), x + 8, y + 28)
        ui.text(('Health %d / %d'):format(hero:hp() or 0, hero:max_hp() or 0), x + 8, y + 46)
        ui.text(('Gold %d'):format(hero:gold() or 0), x + 8, y + 64)
    else
        ui.text('no hero', x + 8, y + 28, {color = ui.rgba(255, 255, 255, 160)})
    end
    -- Alignment inside a box: right-aligned at the panel's right edge.
    ui.text(gear.version, x, y + 6, {width = w - 8, align = 'right', color = ui.rgba(255, 255, 255, 140)})
end

local function images()
    local x, y = 290, 16
    if icon then
        ui.image(icon, x, y)                                        -- its own size
        ui.image(icon, x + 72, y, 32, 32)                           -- scaled down
        ui.image(icon, x + 112, y, 64, 64, {color = ui.rgba(255, 255, 255, 110)})   -- translucent
    end
    if portrait then
        -- The same portrait in a 90 x 60 box: stretched, contained (aspect kept, centered), covering (cropped).
        local bx = x + 186
        for i, fit in ipairs({'stretch', 'contain', 'cover'}) do
            local px = bx + (i - 1) * 98
            ui.rect(px, y, 90, 60, ui.rgba(40, 40, 60, 160))
            ui.image(portrait, px, y, 90, 60, {fit = fit})
            ui.text(fit, px, y + 62, {width = 90, align = 'center', background = PANEL, padding = 2})
        end
    end
end

local function inventory_note(state)
    local inv = ui.window('UI_WND_INVENTORY')
    if inv and inv.drawn then
        -- Drawn in the inventory's frame: its own coordinates, and it moves with the window (SacredBild's HUD layout).
        ui.text('Drawn by the uidemo mod in the inventory\'s frame', inv.x + 8, inv.y - 18,
            {window = 'UI_WND_INVENTORY', background = PANEL, padding = 3, color = GOLD})
    end
end

local function fonts()
    for i, f in ipairs({0, 1, 2, 4, 8, 0x10, 0x20}) do
        local bx, by = 600, 130 + (i - 1) * 40
        ui.frame(bx, by, 300, 34, EDGE)
        ui.text(('flags 0x%x'):format(f), bx, by, {font = 1, width = 300, height = 34, flags = f})
    end
    for id = 0, ui.fonts() - 1 do
        ui.text(('font %d: The quick brown fox 0123'):format(id), 16, 130 + id * 26, {font = id, background = PANEL})
    end
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then return end
    local hero = gear.world.hero()
    if cfg.panel then panel(hero) end
    if cfg.images then images() end
    if cfg.inventory then inventory_note(state) end
    if cfg.fonts then fonts() end
    -- The top layer lies over the escape menu and message boxes too.
    ui.text('uidemo: top layer', 0, 748, {width = 1024, align = 'center', layer = 'top',
        color = ui.rgba(255, 255, 255, 200), background = ui.rgba(0, 0, 0, 120), padding = 2})
    -- The world layer lies under every window (here: under the minimap and the party portraits).
    ui.rect(900, 120, 124, 40, ui.rgba(120, 0, 0, 140), {layer = 'world'})
end)

-- The self test.
if not gear.auto then return end
local A = gear.auto
local results = {passed = 0, failed = 0, steps = {}}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function snap(name)
    local ok, err = A.screenshot('uidemo-' .. name)
    results.steps[#results.steps + 1] = {name = name, state = ui.state(), shot = ok and true or err}
    check('screenshot ' .. name, ok, err)
end

gear.task(function()
    check('main menu', A.wait_for_menu(120))
    if not A.wait_for_game(180) then
        check('game with a hero', false)
    else
        gear.wait(4)
        local w, h = ui.text_size('Sacred Gear')
        check('text measures', w and w > 0, ('%s x %s'):format(tostring(w), tostring(h)))
        check('images found', icon ~= nil and portrait ~= nil)
        local info = icon and ui.image_info(icon)
        results.icon = info
        snap('hud')
        cfg.fonts = true
        gear.wait(0.5)
        snap('fonts')
        cfg.fonts = false
        A.press('i')
        gear.wait(cfg.settle)
        print(gear.json.encode(ui.state()))
        local inv = ui.window('UI_WND_INVENTORY')
        check('inventory drawn', inv and inv.drawn)
        results.inventory = inv
        snap('inventory')
        A.press('i')
        gear.wait(cfg.settle)
        A.press('escape')
        gear.wait(cfg.settle)
        check('escape menu', ui.state().escape_menu)
        snap('escape')
        A.press('escape')
        gear.wait(cfg.settle)
    end
    A.write('uidemo.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end)
