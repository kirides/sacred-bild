-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=theme,rmltest-frames: screenshots of the thick frame and the game buttons in their states
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local doc, checks, fonts
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err; doc, err = ui.rml.load('ui/frames.rml')
        if not doc then print('load failed ' .. tostring(err)) end
    end
    if not checks then checks = ui.rml.load('ui/checks.rml', {model = {flag = false}}) end
    if not fonts then fonts = ui.rml.load('ui/fonts.rml') end
    if fonts then fonts:draw({layer = 'top'}) end
    if doc then doc:draw({layer = 'top'}) end
    if checks then checks:draw({layer = 'top'}) end
end)
local function at(sel)
    local r = doc:query(sel):rect()
    return A.ui_to_pixels(r.x + r.w / 2, r.y + r.h / 2)
end
gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    A.move(1500, 800); gear.wait(0.5)
    A.screenshot('frames-1-base')
    local ff = ui.rml.fonts()
    for _, k in ipairs({'default', 'game', 'start'}) do print('font', k, ff[k].family, ff[k].real, ff[k].file) end
    print('game_font', ui.rml.game_font())
    for _, id in ipairs({'#gb-wide', '#gb-narrow', '#gb-mid', '#gb-dis', '#gb-pad', '#gb-plain'}) do
        local r = doc:query(id):rect()
        print('rect', id, r.x, r.y, r.w, r.h)
    end
    A.move(at('#gb-mid')); gear.wait(0.7)
    A.screenshot('frames-2-hover-mid')
    A.down(); gear.wait(0.5)
    A.screenshot('frames-3-pressed-mid')
    A.up(); gear.wait(0.3)
    A.move(at('#gb-dis')); gear.wait(0.7)
    A.screenshot('frames-4-hover-disabled')
    A.pad(1, 0, 0); gear.wait(0.8)
    A.screenshot('frames-5-pad')
    -- check boxes and radio buttons
    for _, id in ipairs({'#r1', '#c1', '#c4'}) do local r = checks:query(id):rect(); print('crect', id, r.x, r.y, r.w, r.h) end
    local function px(sel) local r = checks:query(sel):rect(); return A.ui_to_pixels(r.x + r.w / 2, r.y + r.h / 2) end
    local function st(sel) local e = checks:query(sel); return tostring(e:attr('checked')) end
    A.move(1500, 850); gear.wait(0.5)
    A.screenshot('checks-1-base')
    print('before', st('#r1'), st('#r2'), st('#r3'), st('#c1'), st('#c2'), st('#c3'))
    A.click(px('#c1')); gear.wait(0.5)
    print('c1 only', st('#c1'))
    A.click(px('#r2')); gear.wait(0.5)
    print('r2 clicked', st('#r1'), st('#r2'), st('#r3'))
    A.click(px('#r1')); gear.wait(0.5)
    print('r1 clicked', st('#r1'), st('#r2'), st('#r3'))
    A.click(px('#c1')); gear.wait(0.5)
    A.click(px('#c3')); gear.wait(0.5)
    A.click(px('#c4')); gear.wait(0.5)
    print('after ', st('#r1'), st('#r2'), st('#r3'), st('#c1'), st('#c2'), st('#c3'), st('#c4'))
    A.screenshot('checks-2-clicked')
    -- the cursor sprite hangs down to the right of its tip: hover with the tip at the lower right of the orb so it does not cover it
    local function pxo(sel) local x, y = px(sel); return x + 13, y + 13 end
    A.move(pxo('#c2')); gear.wait(0.7)
    A.screenshot('checks-3-hover-checked')
    A.move(pxo('#r2')); gear.wait(0.7)
    A.screenshot('checks-4-hover-plain')
    A.down(); gear.wait(0.4)
    A.screenshot('checks-5-pressed')
    A.up(); gear.wait(0.3)
    A.quit(0)
end)
