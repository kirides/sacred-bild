-- Shows what gear.ui can do: a translucent panel with the hero's values, a widget gallery (buttons, checkboxes, radio
-- buttons, a dropdown, sliders, the game's images in the three fit modes and its fonts, tooltips) in a panel that can
-- be dragged and closed (F9 toggles it), a note that follows the inventory window, and the world and top layers.
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

-- The widget gallery: every widget gear.ui has, in a panel that can be dragged by its title and closed (F9 or the
-- 'Widgets' button brings it back).
local G = {clicks = 0, sound = true, music = false, difficulty = 2, fit = 2, alpha = 255, size = 96, font = 1}
local FITS = {'stretch', 'contain', 'cover'}

local function gallery()
    local p = ui.panel('gallery', 'Gear widgets (F9)', 300, 16, 440, 372, {tooltip = 'Drag me by the title'})
    if not p.open then
        if ui.button('reopen', 300, 16, 90, 22, 'Widgets', {tooltip = 'Open the widget gallery'}) then
            ui.show_panel('gallery', true)
        end
        return
    end
    local x, y = p.content_x, p.content_y
    -- Buttons
    ui.label('l-buttons', 'Buttons', x, y, {color = ui.COLORS.accent})
    if ui.button('btn', x, y + 18, 110, 22, 'Click me', {tooltip = 'Counts the clicks'}) then G.clicks = G.clicks + 1 end
    if ui.button('btn-icon', x + 118, y + 18, 130, 22, 'With an icon', {image = icon, tooltip = 'A button with a game image'}) then
        G.clicks = G.clicks + 10
    end
    ui.text(('clicked %d'):format(G.clicks), x + 258, y + 18, {height = 22, valign = 'middle'})
    -- Checkboxes
    y = y + 50
    ui.label('l-checks', 'Checkboxes', x, y, {color = ui.COLORS.accent})
    local _
    _, G.sound = ui.checkbox('sound', x, y + 18, 'Sound effects', G.sound, {tooltip = 'A checkbox'})
    _, G.music = ui.checkbox('music', x + 140, y + 18, 'Music', G.music)
    ui.checkbox('disabled', x + 230, y + 18, 'Disabled', true, {enabled = false, tooltip = 'opts.enabled = false'})
    -- Radio buttons
    y = y + 46
    ui.label('l-radio', 'Radio buttons', x, y, {color = ui.COLORS.accent})
    _, G.difficulty = ui.radio_group('difficulty', x, y + 18, {'Bronze', 'Silver', 'Gold', 'Platinum'},
        G.difficulty, {horizontal = true, tooltips = {'easy', 'normal', 'hard', 'harder'}})
    -- Dropdown, slider
    y = y + 46
    ui.label('l-drop', 'Dropdown and sliders', x, y, {color = ui.COLORS.accent})
    ui.text('Image fit', x, y + 18, {height = 18, valign = 'middle'})
    -- Image (drawn before the dropdown: the open list covers it)
    local px, py = x + 290, y + 18
    ui.rect(px, py, G.size, G.size * 0.6, ui.rgba(40, 40, 60, 160))
    if portrait then
        ui.image(portrait, px, py, G.size, G.size * 0.6, {fit = FITS[G.fit], color = ui.rgba(255, 255, 255, G.alpha)})
    end
    ui.text('Opacity', x, y + 46, {height = 16, valign = 'middle'})
    _, G.alpha = ui.slider('alpha', x + 70, y + 46, 150, 0, 255, G.alpha, {step = 5, tooltip = 'Drag the knob'})
    ui.text('Size', x, y + 70, {height = 16, valign = 'middle'})
    _, G.size = ui.slider('size', x + 70, y + 70, 150, 48, 128, G.size, {step = 4})
    _, G.fit = ui.dropdown('fit', x + 70, y + 18, 150, FITS, G.fit, {tooltip = 'How the image fills its box'})
    -- Text and fonts
    y = y + 100
    ui.label('l-text', 'Text', x, y, {color = ui.COLORS.accent})
    _, G.font = ui.radio_group('font', x, y + 18, {'1', '2', '3', '4', '5', '6', '7'}, G.font, {horizontal = true})
    ui.text('The quick brown fox, font ' .. G.font, x, y + 40, {font = G.font, color = 0xFFFFE0A0})
    ui.label('hover', 'Rest the cursor here for a tooltip', x, y + 76,
        {tooltip = 'Tooltips work on every widget', color = 0xFFB0B0B0})
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
    if cfg.images then gallery() end
    if ui.key_pressed(cfg.key or 'f9') then ui.show_panel('gallery', not ui.panel_open('gallery')) end
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
    results.steps[#results.steps + 1] = {name = name, shot = ok and true or err}
    check('screenshot ' .. name, ok, err)
end

-- The cursor to a UI layout point without clicking; a click there.
local function hover(x, y)
    local px, py = A.ui_to_pixels(x, y)
    A.move(px, py)
    gear.wait(0.1)
end
local function click(x, y)
    A.ui_click(x, y)
    gear.wait(0.4)
end

gear.task(function()
    check('main menu', A.wait_for_menu(120))
    if not A.wait_for_game(180) then
        check('game with a hero', false)
    else
        ui.reset_layout('uidemo:')     -- the gallery where the test expects it
        gear.wait(4)
        local w, h = ui.text_size('Sacred Gear')
        check('text measures', w and w > 0, ('%s x %s'):format(tostring(w), tostring(h)))
        check('images found', icon ~= nil and portrait ~= nil)
        snap('hud')
        local hero = gear.world.hero()
        local hx, hy = hero:position()

        -- The gallery's content starts at (308, 44); rows 50, 46, 46, 100 apart (see gallery()).
        local cx, cy = 308, 44
        hover(cx + 12, cy + 242 + 76 + 6)
        gear.wait(1.0)
        snap('tooltip')
        click(cx + 20, cy + 29)
        check('button', G.clicks == 1, G.clicks)
        click(cx + 147, cy + 50 + 26)
        check('checkbox', G.music == true, tostring(G.music))
        snap('checkbox')
        click(cx + 7, cy + 96 + 26)
        check('radio', G.difficulty == 1, G.difficulty)
        click(cx + 100, cy + 142 + 27)
        check('dropdown opens', true)
        snap('dropdown-open')
        click(cx + 100, cy + 142 + 36 + 1 + 36 + 9)
        check('dropdown picks cover', G.fit == 3, G.fit)
        click(cx + 70 + 5 + 140 * 0.4, cy + 142 + 54)
        check('slider', G.alpha < 255 and G.alpha > 0, G.alpha)
        gear.wait(0.3)
        snap('widgets')
        -- Drag the panel by its title, then close it.
        local bx, by = A.ui_to_pixels(420, 26)
        A.move(bx, by); gear.wait(0.1); A.down(); gear.wait(0.2)
        for i = 1, 5 do
            local mx, my = A.ui_to_pixels(420 + i * 20, 26 + i * 16)
            A.move(mx, my); gear.wait(0.1)
        end
        A.up(); gear.wait(0.4)
        snap('dragged')
        click(300 + 100 + 440 - 12, 16 + 80 + 11)
        check('panel closed', not ui.panel_open('gallery'))
        snap('closed')
        click(320, 26)
        check('panel reopened', ui.panel_open('gallery'))
        local hx2, hy2 = hero:position()
        check('clicks did not reach the world (the hero stayed)', hx2 == hx and hy2 == hy, ('%d,%d -> %d,%d'):format(hx, hy, hx2, hy2))
    end
    ui.reset_layout('uidemo:')
    A.write('uidemo.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end)