-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=rmltest-windrag [-gear-window=1600x900]
-- A kit window (title and id, so a panel named after the id) and a second one with `panel = false` (fixed).
local kit, ui, A = gear.kit, gear.ui, gear.auto
local doc, closed = nil, false

local function markup()
    return kit.page(kit.window{id = 'win', title = 'Drag me', x = 200, y = 160, width = 300, close = 'x', body = kit.text('Drag the title bar.')}
        .. kit.window{id = 'fixed', title = 'Fixed', panel = false, x = 600, y = 160, width = 200, body = kit.text('I stay.')})
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc and not closed then
        doc = ui.rml.load_string(markup(), {})
        if doc then doc:on('#x', 'click', function() closed = true; doc:close(); doc = nil end) end
    end
    if doc then doc:draw({layer = 'top'}) end
end)

if not A then return end
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end
local function panel(id)
    for _, p in ipairs(ui.panels()) do if p.key == 'rmltest-windrag:' .. id then return p end end
end
local function move_ui(x, y) local px, py = A.ui_to_pixels(x, y); A.move(px, py) end
local function drag(fx, fy, tx, ty, release)
    move_ui(fx, fy); gear.wait(0.4)
    A.down(); gear.wait(0.3)
    for i = 1, 10 do move_ui(fx + (tx - fx) * i / 10, fy + (ty - fy) * i / 10); gear.wait(0.06) end
    gear.wait(0.3)
    if release ~= false then A.up(); gear.wait(0.5) end
end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    for _ = 1, 20 do if panel('win') then break end gear.wait(0.5) end
    local p = panel('win')
    check('the window is a panel', p ~= nil)
    check('the fixed window is none', panel('fixed') == nil)
    if not p then return A.quit(1) end
    local sl, st, sr, sb = ui.state().screen_left, ui.state().screen_top, ui.state().screen_right, ui.state().screen_bottom
    A.screenshot('windrag-0-home')
    local x0, y0 = p.x, p.y
    -- a click on the bar moves nothing and keeps nothing
    move_ui(p.x + 60, p.y + 10); gear.wait(0.3); A.down(); gear.wait(0.2); A.up(); gear.wait(0.5)
    p = panel('win')
    check('a click moves nothing', math.abs(p.x - x0) < 0.5 and math.abs(p.y - y0) < 0.5 and ui.layout()['rmltest-windrag:win'] == nil)
    -- drag by the bar
    drag(p.x + 60, p.y + 10, p.x + 60 + 150, p.y + 10 + 90)
    p = panel('win')
    check('dragged by 150,90', math.abs(p.x - (x0 + 150)) < 3 and math.abs(p.y - (y0 + 90)) < 3, ('%.1f,%.1f -> %.1f,%.1f'):format(x0, y0, p.x, p.y))
    local e = ui.layout()['rmltest-windrag:win']
    check('the place is kept in the layout', e ~= nil and e.x ~= nil)
    A.screenshot('windrag-1-moved')
    local x1, y1 = p.x, p.y
    -- a drag from the body moves nothing
    drag(p.x + 60, p.y + p.h - 12, p.x + 160, p.y + p.h + 60)
    p = panel('win')
    check('a drag from the body moves nothing', math.abs(p.x - x1) < 0.5 and math.abs(p.y - y1) < 0.5)
    -- off the screen: clamped
    drag(p.x + 60, p.y + 10, sr + 400, sb + 400)
    p = panel('win')
    check('clamped on the screen (right, bottom)', p.x + p.w <= sr + 0.5 and p.y + p.h <= sb + 0.5 and p.x + p.w >= sr - 1, ('%.1f,%.1f %.0fx%.0f screen %d,%d'):format(p.x, p.y, p.w, p.h, sr, sb))
    A.screenshot('windrag-2-clamped')
    drag(p.x + 60, p.y + 10, sl - 400, st - 400)
    p = panel('win')
    check('clamped on the screen (left, top)', p.x >= sl - 0.5 and p.y >= st - 0.5 and p.x <= sl + 1, ('%.1f,%.1f'):format(p.x, p.y))
    -- the layout editor and the drag agree: reset puts it back
    ui.reset_panel('rmltest-windrag:win'); gear.wait(0.6)
    p = panel('win')
    check('reset puts it back', math.abs(p.x - x0) < 1 and math.abs(p.y - y0) < 1, ('%.1f,%.1f'):format(p.x, p.y))
    -- the fixed window cannot be dragged
    local fx, fy = 600 + 40, 160 + 8
    drag(fx, fy, fx + 100, fy + 100)
    A.screenshot('windrag-3-fixed')
    -- the close button still closes
    p = panel('win')
    local cx, cy = p.x + p.w - 28, p.y + 20
    move_ui(cx, cy); gear.wait(0.3); A.click(A.ui_to_pixels(cx, cy)); gear.wait(0.6)
    check('the close button still works', closed)
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
