-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=rmltest-scale [-gear-window=WxH] [-gear-ui-scale=N]: the same sprites through RmlUi decorators and through
-- gear.ui.image (they must come out pixel-identical at any scale), a 300 unit repeated groove both ways (seams), and gear.ui.native.frame('thick') beside the
-- in-game menu's own frame, shifted left by 320 layout units, for a measurement in layout units.
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local doc
local phase = 0
local function tiles(id, tw, th, x, y, len)
    local at = 0
    while at < len - 0.01 do
        local seg = math.min(tw, len - at)
        ui.image(id, x + at, y, seg, th, {layer = 'top', crop = seg < tw and {0, 0, seg / tw, 1} or nil})
        at = at + tw
    end
end
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        doc = ui.rml.load_string([[<rml><body>
<div style="position:absolute; left:100px; top:100px; width:45px; height:37px; decorator: image(gear:image/280 scale-none left center);"></div>
<div style="position:absolute; left:100px; top:160px; width:300px; height:37px; decorator: image(gear:image/264 repeat-x);"></div>
<div style="position:absolute; left:100px; top:220px; width:40px; height:37px; decorator: image(gear:image/265 scale-none right center);"></div>
</body></rml>]], {theme = false})
    end
    if phase == 1 then ui.rect(0, 0, 1024, 768, 0xFF000000, {layer = 'top'}) end
    if phase >= 1 and doc then doc:draw({layer = 'top'}) end
    if phase >= 1 then
        ui.image(280, 612, 100, 45, 37, {layer = 'top'})
        tiles(264, 38, 37, 612, 160, 300)
        ui.image(265, 612, 220, 40, 37, {layer = 'top'})
        ui.image(280, 100, 300, 45, 37, {layer = 'top'})
        ui.image(280, 612, 300, 45, 37, {layer = 'top'})
    end
    if phase == 3 then
        ui.rect(0, 0, 1024, 768, 0xFFFF00FF, {layer = 'top'})
        ui.native.frame(200, 150, 620.0, 300.0, {style = 'thick', layer = 'top', fill = false, dividers = {140}})
    end
    if phase == 2 then
        ui.native.frame(41.0, 262.0, 300.9, 242.5, {style = 'thick', layer = 'top', fill = false})
    end
end)
gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    A.move(10, 10); gear.wait(0.3)
    A.screenshot('s0')
    phase = 1; gear.wait(0.8)
    A.screenshot('s1')
    A.press('escape'); gear.wait(1.5)
    A.screenshot('s2e')
    phase = 2; gear.wait(0.8)
    A.screenshot('s2')
    phase = 3; gear.wait(0.8)
    A.screenshot('s3')
    A.quit(0)
end)
