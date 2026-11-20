-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=dragdemo,rmltest-drag (single player: the world drop spawns a creature)
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function panel()
    for _, p in ipairs(ui.panels()) do if p.key == 'dragdemo:main' then return p end end
end
local function name_pos(i) local p = panel(); return p.x + 8 + 8 + 55, p.y + 28 + 8 + (i - 1) * 24 + 10 end
local function slot_pos(i) local p = panel(); return p.x + 8 + 140 + 50, p.y + 28 + 8 + (i - 1) * 44 + 19 end

local function move_ui(x, y)
    local px, py = A.ui_to_pixels(x, y)
    A.move(px, py)
end
local function drag(from, to, shot, release)
    move_ui(from[1], from[2]); gear.wait(0.3)
    A.down(); gear.wait(0.2)
    for i = 1, 8 do
        move_ui(from[1] + (to[1] - from[1]) * i / 8, from[2] + (to[2] - from[2]) * i / 8)
        gear.wait(0.08)
    end
    gear.wait(0.4)
    if shot then A.screenshot(shot) end
    if release ~= false then A.up(); gear.wait(0.5) end
end
local function near_count(h)
    local x, y = h:position()
    return #gear.world.creatures_near(x, y, 6000, {limit = 1000})
end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    for _ = 1, 20 do if panel() then break end gear.wait(0.5) end
    local p = panel()
    check('the panel exists', p ~= nil)
    if not p then return A.quit(1) end
    A.screenshot('drag-0-base')
    local hero = gear.world.hero()
    local hx, hy = hero:position()
    -- 1. a valid slot: highlight and plus badge while dragging, a drop fills it
    drag({name_pos(1)}, {slot_pos(1)}, 'drag-1-valid-hover')
    A.screenshot('drag-2-dropped')
    -- 2. the locked slot: red cross and the reason while dragging; the release drops nothing
    drag({name_pos(2)}, {slot_pos(3)}, 'drag-3-locked-hover')
    A.screenshot('drag-4-locked-released')
    -- 3. Esc cancels
    drag({name_pos(3)}, {slot_pos(2)}, 'drag-5-before-esc', false)
    A.press('escape'); gear.wait(0.4)
    A.up(); gear.wait(0.5)
    A.screenshot('drag-6-after-esc')
    check('Esc cancels the drag and does not open the game menu', not ui.window_open('UI_WND_ESCMENU') and ui.drag_active() == nil)
    -- 4. world drop: spawns the creature at the cursor (single player)
    local before = gear.world.can_change() and near_count(hero) or -1
    local wx, wy = A.ui_to_pixels(640, 520)
    drag({name_pos(4)}, {640, 520}, 'drag-7-world-hover')
    gear.wait(1.5)
    local after = near_count(hero)
    check('single player world', gear.world.can_change())
    check('a drop on the world spawns a creature', after > before, ('%s -> %s'):format(before, after))
    local nx, ny = hero:position()
    check('the press was not forwarded to the game (hero did not walk)', math.abs(nx - hx) < 3 and math.abs(ny - hy) < 3,
        ('%.1f,%.1f -> %.1f,%.1f'):format(hx, hy, nx, ny))
    gear.wait(3); A.screenshot('drag-8-after-world')
    -- 5. the pad: with the controller used last, a click picks up (carry) and the targets are the stops
    A.pad(1, 0, 0); gear.wait(0.5)
    local nx0, flags0 = A.nav()
    move_ui(name_pos(1)); gear.wait(0.3)
    A.click(A.ui_to_pixels(name_pos(1))); gear.wait(0.6)
    print('active drag after pad click', ui.drag_active() ~= nil)
    local n1, f1 = A.nav()
    print('nav before / after the pick up', nx0, flags0, n1, f1)
    A.screenshot('drag-9-pad-carry')
    if ui.drag_active() then A.pad(0, 1, 0); gear.wait(0.5) end
    print('drag after B', ui.drag_active() ~= nil)
    A.screenshot('drag-10-pad-b')
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)







