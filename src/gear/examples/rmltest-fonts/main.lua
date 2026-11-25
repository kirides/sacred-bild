-- The font page of the theme gallery: Sacred.exe -gear-mods=theme,rmltest-fonts shows the game font, the modern font and the start screen font in
-- game; with -gear-auto (and -gear-load=1) it prints which family is real and which a fallback, and takes a screenshot.
local A, ui = gear.auto, gear.ui
local doc
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then doc = ui.rml.load('ui/fonts.rml') end
    if doc then doc:draw({layer = 'top'}) end
end)
if not A then return end
gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    A.move(1500, 850); gear.wait(0.5)
    local f = ui.rml.fonts()
    for _, k in ipairs({'default', 'game', 'start'}) do print('font', k, f[k].family, f[k].real, f[k].file) end
    print('game_font', ui.rml.game_font())
    A.screenshot('fonts-1')
    A.quit(0)
end)
