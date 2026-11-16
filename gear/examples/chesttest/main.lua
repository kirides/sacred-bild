-- A self test of the movable shop windows at a fixed UI scale (the contents must draw inside the window's frame):
--
--     Sacred.exe -gear-auto -gear-load=1 -gear-mods=chesttest -gear-ui-scale=1
--
-- Shows the chest, moves it with the layout editor's call (ui.set_window_position), and checks that the window's drawn
-- bounds (frame + its own layout) lie where the window's frame says, with screenshots of each state for a person to
-- judge (the item grid belongs inside the chest's frame).

if not gear.auto then
    print('no automation (start the game with -gear-auto): nothing to do')
    return
end

local cfg = gear.config
local A = gear.auto
local ui = gear.ui
local results = {passed = 0, failed = 0}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
    return ok
end

local function describe(name)
    local w = ui.window(name)
    if not w then return print(name .. ': none') end
    local b = w.bounds
    print(('%s: rect %d,%d %dx%d frame %.1f,%.1f drawn %s bounds %s'):format(name, w.x, w.y, w.width, w.height,
        w.frame_x, w.frame_y, tostring(w.drawn), b and ('%.0f,%.0f %.0fx%.0f'):format(b.x, b.y, b.w, b.h) or '-'))
    return w
end

local function shot(tag, name)
    gear.wait(cfg.settle)
    A.screenshot('chesttest-' .. tag)
    return describe(name)
end

gear.task(function()
    check('main menu', A.wait_for_menu(cfg.timeout_menu))
    if not A.wait_for_game(cfg.timeout_game) then
        check('game with a hero', false)
        return A.quit(1)
    end
    gear.wait(3)
    print('state: ' .. gear.json.encode(ui.state()))
    check('windows can move', ui.can_move_windows())
    for _, name in ipairs({'UI_WND_CHEST', 'UI_WND_INVENTORY'}) do
        A.show_window(name, true)
    end
    -- Copies of a few backpack items into the chest, so there is content to draw in it.
    local chest, pack = gear.inv.container('chest'), gear.inv.container('backpack')
    local added = 0
    for i, it in ipairs(pack and pack:items() or {}) do
        if i > 8 then break end
        local bytes = gear.inv.export(it.id)
        if bytes and gear.inv.add(chest, bytes) then added = added + 1 end
    end
    check('items copied into the chest', added > 0, added)
    local w = shot('home', 'UI_WND_CHEST')
    check('chest drawn', w and w.drawn)
    describe('UI_WND_INVENTORY')
    local home = w and w.bounds
    local sl, st = ui.state().screen_left, ui.state().screen_top
    -- Moved to a place of its own.
    ui.set_window_position('UI_WND_CHEST', sl + 300, st + 120)
    w = shot('moved', 'UI_WND_CHEST')
    local b = w and w.bounds
    if b and w then
        check('chest bounds follow its frame', math.abs((b.x) - (w.x + w.frame_x)) < 700 and w.frame_x ~= nil)
        check('chest frame is where it was put', math.abs(w.x + w.frame_x - (sl + 300)) < 2 and math.abs(w.y + w.frame_y - (st + 120)) < 2,
            ('window %d,%d + frame %.1f,%.1f, wanted %.0f,%.0f'):format(w.x, w.y, w.frame_x, w.frame_y, sl + 300, st + 120))
    end
    -- A place saved in the layout's virtual 4096 x 4096 screen (the user's: 560,125), as the layout editor restores it.
    local vx, vy = ui.from_virtual(560, 125)
    ui.set_window_position('UI_WND_CHEST', vx, vy)
    w = shot('virtual', 'UI_WND_CHEST')
    print(('virtual 560,125 -> canvas %.1f,%.1f'):format(vx, vy))
    -- Mouse over the chest's cells: hover, then an item picked up and held over empty cells.
    local cw = ui.window('UI_WND_CHEST')
    local ox, oy = cw.frame_x + cw.bounds.x - cw.frame_x, cw.bounds.y
    local gx, gy = cw.bounds.x + 20, cw.bounds.y + 20      -- inside the first cell (cells are 32 px; grid starts at the bounds)
    local px, py = A.ui_to_pixels(gx, gy)
    A.move(px, py); gear.wait(1.5)
    A.screenshot('chesttest-hover')
    A.click(px, py); gear.wait(0.5)
    px, py = A.ui_to_pixels(gx + 256, gy + 96)
    A.move(px, py); gear.wait(0.8)
    A.screenshot('chesttest-held')
    A.click(px, py); gear.wait(0.5)
    A.screenshot('chesttest-dropped')
    -- The layout editor's overlays (needs -gear-mods=layout,chesttest).
    ui.set_edit_mode(true)
    shot('edit', 'UI_WND_CHEST')
    ui.set_edit_mode(false)
    ui.reset_window('UI_WND_CHEST')
    A.show_window('UI_WND_CHEST', false)
    A.show_window('UI_WND_INVENTORY', false)
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    A.quit(results.failed == 0 and 0 or 1)
end)
