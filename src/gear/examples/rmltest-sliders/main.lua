-- The slider page of the theme gallery: Sacred.exe -gear-mods=theme,rmltest-sliders shows it in game; with -gear-auto (and -gear-load=1) a self test
-- clicks in the track (the value jumps), the minus and plus buttons (a step), drags the knob and checks the values in the data model.
local A, ui = gear.auto, gear.ui
local doc
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err
        doc, err = ui.rml.load('ui/sliders.rml', {model = {effects = 50, voice = 30, music = 70}})
        if not doc then print('load failed ' .. tostring(err)) end
    end
    if doc then doc:draw({layer = 'top'}) end
end)
if not A then return end
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end
-- where the knob's centre is for value v (caps 37 and 40 wide, the knob 30)
local function knob_px(sel, v)
    local r = doc:query(sel):rect()
    local track = r.w - 37 - 40 - 30
    return A.ui_to_pixels(r.x + 37 + 15 + track * v / 100, r.y + r.h / 2)
end
local function minus_px(sel) local r = doc:query(sel):rect(); return A.ui_to_pixels(r.x + 18, r.y + r.h / 2) end
local function plus_px(sel) local r = doc:query(sel):rect(); return A.ui_to_pixels(r.x + r.w - 20, r.y + r.h / 2) end
local function val(sel) return tonumber(doc:query(sel):value()) end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    A.move(1500, 850); gear.wait(0.5)
    A.screenshot('sliders-1-base')
    for _, id in ipairs({'#s1', '#s2', '#s3', '#s4', '#s5'}) do local r = doc:query(id):rect(); print('srect', id, r.x, r.y, r.w, r.h) end
    check('model values reach the sliders', math.abs(val('#s1') - 50) < 0.6 and math.abs(val('#s2') - 30) < 0.6, ('%s %s'):format(val('#s1'), val('#s2')))
    -- a press in the track jumps to that value
    A.click(knob_px('#s1', 80)); gear.wait(0.5)
    check('a click in the track jumps (80)', math.abs(val('#s1') - 80) <= 2, val('#s1'))
    check('the model follows', math.abs((doc:get('effects') or -1) - val('#s1')) < 0.6, doc:get('effects'))
    A.click(knob_px('#s1', 10)); gear.wait(0.5)
    check('a click near the start jumps (10)', math.abs(val('#s1') - 10) <= 2, val('#s1'))
    -- minus and plus step by one
    local before = val('#s1')
    A.click(plus_px('#s1')); gear.wait(0.5)
    check('plus steps up by one', math.abs(val('#s1') - (before + 1)) < 0.6, val('#s1'))
    A.click(minus_px('#s1')); gear.wait(0.5)
    A.click(minus_px('#s1')); gear.wait(0.5)
    check('minus steps down by one', math.abs(val('#s1') - (before - 1)) < 0.6, val('#s1'))
    -- the short one steps by its step (5)
    local b4 = val('#s4')
    A.click(plus_px('#s4')); gear.wait(0.5)
    check('the short slider steps by 5', math.abs(val('#s4') - (b4 + 5)) < 0.6, val('#s4'))
    -- drag the knob of the long one
    local x0, y0 = knob_px('#s5', val('#s5'))
    local x1 = knob_px('#s5', 60)
    A.move(x0, y0); gear.wait(0.3); A.down(); gear.wait(0.3)
    for i = 1, 6 do A.move(x0 + (x1 - x0) * i / 6, y0); gear.wait(0.1) end
    A.up(); gear.wait(0.4)
    check('dragging the knob sets the value (60)', math.abs(val('#s5') - 60) <= 3, val('#s5'))
    A.screenshot('sliders-2-after')
    -- hovering the minus and plus buttons
    A.move(plus_px('#s2')); gear.wait(0.7)
    A.screenshot('sliders-3-hover-plus')
    -- the pad: the D-pad nudges the slider under the pad focus
    A.move(knob_px('#s3', 70)); gear.wait(0.4)
    local before_pad = val('#s3')
    A.pad(1, 0, 0); gear.wait(0.5)
    A.pad(1, 0, 0); gear.wait(0.5)
    A.pad(-1, 0, 0); gear.wait(0.5)
    check('the D-pad nudges the slider under the focus (2 per press)', math.abs(val('#s3') - (before_pad + 2)) < 0.6, ('%s -> %s'):format(before_pad, val('#s3')))
    A.screenshot('sliders-4-pad')
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
