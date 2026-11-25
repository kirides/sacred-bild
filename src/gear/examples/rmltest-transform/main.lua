-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=rmltest-transform [-gear-window=1600x900]
-- Three buttons in a box scaled to 40% (transform: scale, origin top left): a mouse click on each one's drawn centre must reach that button (the
-- layout rectangles of the scaled buttons overlap the neighbours' drawn places and lie beyond the box).
local ui, A = gear.ui, gear.auto
local doc
local hits = {}

local MARKUP = [[<rml><body>
<div id="box" style="position:absolute; left:200px; top:200px; width:600px; height:100px; transform: scale(0.4); transform-origin: left top;">
<button id="a" style="position:absolute; left:0px; top:0px; width:200px; height:100px;">A</button>
<button id="b" style="position:absolute; left:200px; top:0px; width:200px; height:100px;">B</button>
<button id="c" style="position:absolute; left:400px; top:0px; width:200px; height:100px;">C</button>
</div></body></rml>]]

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        doc = ui.rml.load_string(MARKUP, {})
        if doc then
            for _, id in ipairs({'a', 'b', 'c'}) do doc:on('#' .. id, 'click', function() hits[#hits + 1] = id end) end
        end
    end
    if doc then doc:draw({layer = 'top'}) end
end)

if not A then return end
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end
local function move_ui(x, y) local px, py = A.ui_to_pixels(x, y); A.move(px, py) end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(3)
    A.screenshot('transform-0')
    -- drawn: a 200..280, b 280..360, c 360..440 (x), 200..240 (y)
    for i, id in ipairs({'a', 'b', 'c'}) do
        hits = {}
        local cx, cy = 200 + (i - 1) * 80 + 40, 220
        move_ui(cx, cy); gear.wait(0.4)
        A.down(); gear.wait(0.15); A.up(); gear.wait(0.5)
        check('a click on the drawn ' .. id .. ' reaches ' .. id, #hits == 1 and hits[1] == id, table.concat(hits, ','))
    end
    -- the layout place of c (x 400..600 at 100% size) is outside the drawn box: a click there reaches nothing
    hits = {}
    move_ui(500, 220); gear.wait(0.4)
    A.down(); gear.wait(0.15); A.up(); gear.wait(0.5)
    check('a click where the layout has a button, but it is not drawn, reaches nothing', #hits == 0, table.concat(hits, ','))
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
