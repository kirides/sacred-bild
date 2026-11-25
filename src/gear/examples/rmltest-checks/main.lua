-- The check box page of the theme gallery: Sacred.exe -gear-mods=theme,rmltest-checks shows it in game; with -gear-auto (and -gear-load=1) a self test
-- clicks the orbs (a radio group, check boxes, a data-bound one) and takes screenshots of the hover (the cursor tip lower right of the orb).
local A, ui = gear.auto, gear.ui
local checks
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not checks then checks = ui.rml.load('ui/checks.rml', {model = {flag = false}}) end
    if checks then checks:draw({layer = 'top'}) end
end)
if not A then return end
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end
gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    local function px(sel) local r = checks:query(sel):rect(); return A.ui_to_pixels(r.x + r.w / 2, r.y + r.h / 2) end
    local function pxo(sel) local x, y = px(sel); return x + 13, y + 13 end
    local function st(sel) return checks:query(sel):attr('checked') ~= nil end
    A.move(1500, 850); gear.wait(0.5)
    A.screenshot('checks-1-base')
    check('initial: Hoch and Checked are on', st('#r3') and st('#c2') and not st('#r1') and not st('#c1'))
    A.click(px('#c1')); gear.wait(0.5)
    check('a check box toggles', st('#c1'))
    A.click(px('#r2')); gear.wait(0.5)
    check('a radio takes the group', st('#r2') and not st('#r3'))
    A.click(px('#r1')); gear.wait(0.5)
    check('another radio takes it again', st('#r1') and not st('#r2'))
    A.click(px('#c3')); gear.wait(0.5)
    check('the bound one reaches the model', checks:get('flag') == true, checks:get('flag'))
    A.click(px('#c4')); gear.wait(0.5)
    check('a disabled one does not toggle', st('#c4'))
    A.screenshot('checks-2-clicked')
    A.move(pxo('#c2')); gear.wait(0.7)
    A.screenshot('checks-3-hover-checked')
    A.move(pxo('#r2')); gear.wait(0.7)
    A.screenshot('checks-4-hover-plain')
    A.down(); gear.wait(0.4)
    A.screenshot('checks-5-pressed')
    A.up(); gear.wait(0.3)
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
