-- A layout editor, like the edit mode of other games: F7 (or its button) unlocks the UI. Then
--  * every open game window (inventory, character, skills, minimap, taskbar, shops, log book ...) shows a handle:
--    drag it to move the window anywhere on the screen;
--  * every mod panel shows its id ("mod_id:panel_id") and, if it can be resized, its grip.
-- Places are kept in mods\gear_layout.json in a virtual 4096 x 4096 screen, so they stay when the resolution changes.
-- Moving the game's windows needs SacredBild (its UI frames); the mod panels move without it.
--
-- Self test, run twice: Sacred.exe -gear-auto -gear-load=1 -gear-mods=layout -gear-shared-input
-- The first run moves the inventory and the editor's toolbar and quits; the second checks they came back where they
-- were left, takes screenshots and resets the layout.

local cfg = gear.config
local ui = gear.ui
local C = ui.COLORS

local drag = nil            -- {name, grab_x, grab_y} while a game window is dragged

local function window_rect(w)
    return w.x + w.frame_x, w.y + w.frame_y, w.width, w.height
end

local function handles(state)
    for _, w in ipairs(ui.windows()) do
        if w.drawn and w.movable and w.width > 0 and w.height > 0 and w.name ~= '' then
            local x, y, ww, hh = window_rect(w)
            local top = {layer = 'top'}
            local r = ui.region('win:' .. w.name, x, y, ww, hh, {layer = 'top', tooltip = 'Drag to move ' .. w.name})
            ui.rect(x, y, ww, hh, r.hover and 0x5050A0FF or 0x30000000, top)
            ui.frame(x, y, ww, hh, r.hover and C.accent or C.edge, 2, top)
            ui.text(w.name, x + 6, y + 4, {font = 7, color = C.accent, background = 0xC0000000, padding = 2, layer = 'top'})
            if r.pressed then
                local mx, my = ui.mouse()
                drag = {name = w.name, grab_x = mx - x, grab_y = my - y}
            end
            if drag and drag.name == w.name then
                if r.held then
                    local mx, my = ui.mouse()
                    ui.set_window_position(w.name, mx - drag.grab_x, my - drag.grab_y)
                else
                    drag = nil
                end
            end
        end
    end
end

local function toolbar(state)
    local p = ui.panel('toolbar', 'Layout', 400, 300, 240, 132, {layer = 'top', closable = false,
        resizable = true, min_width = 240, min_height = 132, tooltip = 'Drag me too'})
    if not p.open then return end
    local x, y = p.content_x, p.content_y
    if ui.can_move_windows() then
        ui.text('Drag the windows and panels.', x, y, {font = 7, layer = 'top'})
    else
        ui.text('Game windows need SacredBild.', x, y, {font = 7, color = 0xFFFF8080, layer = 'top'})
    end
    if ui.button('done', x, y + 22, 106, 22, 'Done (F7)', {layer = 'top'}) then ui.set_edit_mode(false) end
    if ui.button('reset-game', x + 116, y + 22, 106, 22, 'Reset windows', {layer = 'top',
        tooltip = 'The game windows back to their places'}) then
        ui.reset_layout('game:')
    end
    if ui.button('reset-all', x, y + 52, 222, 22, 'Reset everything', {layer = 'top',
        tooltip = 'Game windows and every mod panel'}) then
        ui.reset_layout()
    end
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then return end
    if ui.key_pressed(cfg.key) then ui.set_edit_mode(not ui.edit_mode()) end
    if ui.edit_mode() then
        handles(state)
        toolbar(state)
    elseif ui.button('edit', state.screen_left + 4, state.screen_top + 4, 80, 20, 'Edit layout',
        {tooltip = 'Move windows and panels (F7)'}) then
        ui.set_edit_mode(true)
    end
end)

---------------------------------------------------------------------------------------------------------------
-- Self test
---------------------------------------------------------------------------------------------------------------
if not gear.auto then return end
local A = gear.auto
local results = {passed = 0, failed = 0}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function drag_ui(x0, y0, x1, y1)
    local px, py = A.ui_to_pixels(x0, y0)
    A.move(px, py); gear.wait(0.15); A.down(); gear.wait(0.2)
    for i = 1, 6 do
        local mx, my = A.ui_to_pixels(x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6)
        A.move(mx, my); gear.wait(0.1)
    end
    A.up(); gear.wait(0.5)
end

local function inventory()
    local w = ui.window('UI_WND_INVENTORY')
    if w then return w, w.x + w.frame_x, w.y + w.frame_y end
end

gear.task(function()
    check('main menu', A.wait_for_menu(120))
    if not A.wait_for_game(180) then
        check('game with a hero', false)
        return A.quit(1)
    end
    gear.wait(3)
    check('game windows can move (SacredBild)', ui.can_move_windows())
    A.press('i')
    gear.wait(1.5)
    local lay = ui.layout()
    local second = lay['layout:toolbar'] ~= nil and lay['game:UI_WND_INVENTORY'] ~= nil
    results.run = second and 'second' or 'first'
    print('run: ' .. results.run .. ', layout: ' .. gear.json.encode(lay))
    if not second then
        local w, x, y = inventory()
        check('inventory open', w and w.drawn)
        ui.set_edit_mode(true)
        gear.wait(0.5)
        A.screenshot('layout-edit')
        drag_ui(x + 300, y + 120, x + 300 + 250, y + 120 - 200)
        local _, x2, y2 = inventory()
        check('inventory moved', x2 and math.abs(x2 - (x + 250)) < 3 and math.abs(y2 - (y - 200)) < 3,
            ('%s,%s -> %s,%s'):format(x, y, tostring(x2), tostring(y2)))
        A.screenshot('layout-moved')
        drag_ui(420, 310, 420 + 300, 310 + 180)     -- the toolbar's title bar
        gear.wait(0.5)
        drag_ui(700 + 240 - 7, 480 + 132 - 7, 700 + 240 - 7 + 80, 480 + 132 - 7 + 40)   -- its resize grip
        gear.wait(0.5)
        local t = ui.layout()['layout:toolbar']
        local tw = t and t.w and select(1, ui.from_virtual(t.w, 0)) - select(1, ui.from_virtual(0, 0))
        check('toolbar resized', tw and math.abs(tw - 320) < 4, tostring(tw))
        A.screenshot('layout-toolbar-moved')
        ui.set_edit_mode(false)
        gear.wait(0.8)
        A.screenshot('layout-done')
        local l2 = ui.layout()
        check('stored', l2['game:UI_WND_INVENTORY'] ~= nil and l2['layout:toolbar'] ~= nil, gear.json.encode(l2))
        A.ui_click(x + 250 + 300, y - 200 + 120)    -- a click into the moved inventory reaches it
        gear.wait(0.5)
        A.screenshot('layout-clicked')
    else
        local w, x, y = inventory()
        local e = lay['game:UI_WND_INVENTORY']
        local ex, ey = ui.from_virtual(e.x, e.y)
        check('inventory where it was left', x and math.abs(x - ex) < 3 and math.abs(y - ey) < 3,
            ('%s,%s expected %.0f,%.0f'):format(tostring(x), tostring(y), ex, ey))
        A.screenshot('layout-restored')
        ui.set_edit_mode(true)
        gear.wait(0.5)
        A.screenshot('layout-restored-edit')
        ui.reset_layout()
        ui.set_edit_mode(false)
        gear.wait(0.8)
        local _, x3, y3 = inventory()
        check('reset puts it back', x3 and (math.abs(x3 - x) > 3 or math.abs(y3 - y) > 3), ('%s,%s -> %s,%s'):format(tostring(x), tostring(y), tostring(x3), tostring(y3)))
        A.screenshot('layout-reset')
    end
    A.press('i')
    gear.wait(0.5)
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    A.quit(results.failed == 0 and 0 or 1)
end)
