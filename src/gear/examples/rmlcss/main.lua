local A = gear.auto
local d
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not d then d = gear.ui.rml.load('ui/c.rml', {theme = false}); gear.log('loaded ' .. tostring(d)) end
    if d then d:draw({layer = 'top'}) end
end)
if A then gear.task(function() if not A.wait_for_game(180) then return A.quit(1) end gear.wait(6) A.screenshot('rmlcss') A.quit(0) end) end
