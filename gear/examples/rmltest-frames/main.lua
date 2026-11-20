-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=theme,rmltest-frames: screenshots of the thick frame and the game buttons in their states
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local doc
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err; doc, err = ui.rml.load('ui/frames.rml')
        if not doc then print('load failed ' .. tostring(err)) end
    end
    if doc then doc:draw({layer = 'top'}) end
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
    A.quit(0)
end)
