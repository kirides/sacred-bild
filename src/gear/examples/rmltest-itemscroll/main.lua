-- Item models in an RmlUi list (gear:item/<type>) and element:scroll(): Sacred.exe -gear-mods=theme,rmltest-itemscroll shows it in game;
-- with -gear-auto (and -gear-load=1) a self test scrolls the list to its end and takes screenshots (the icons at the top, then the end).
local A, ui = gear.auto, gear.ui
local doc
local function rows()
    local out = {}
    local T = gear.data.object_types
    for i, r in ipairs(T:search('SWORD', 14) or {}) do
        out[#out + 1] = {src = 'gear:item/' .. r.id, name = ('%d  %s'):format(r.id, tostring(r.name or r.label))}
    end
    return out
end
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err
        doc, err = ui.rml.load('ui/list.rml', {model = {rows = rows()}})
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
gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    A.move(1500, 850); gear.wait(0.5)
    local list = doc:query('#list')
    local top, height, client = list:scroll()
    check('the list reports its size', top == 0 and height and client and height > client, ('%s %s %s'):format(top, height, client))
    A.screenshot('itemscroll-1-top')
    local t2 = list:scroll('end')
    gear.wait(0.5)
    local top2, h2, c2 = list:scroll()
    check('scroll("end") reaches the bottom', top2 and math.abs(top2 - (h2 - c2)) < 1.5, ('%s of %s'):format(top2, h2 - c2))
    A.screenshot('itemscroll-2-end')
    list:scroll('start'); gear.wait(0.3)
    -- a data-resizable panel takes the size the layout gives it (what the layout editor's grip and popup do)
    check('the panel is listed as resizable', (function() for _, p in ipairs(ui.panels()) do if p.key == 'rmltest-itemscroll:w' then return p.resizable and p.min_width == 200 end end end)())
    ui.set_panel_rect('rmltest-itemscroll:w', 80, 60, 320, 240)
    gear.wait(0.7)
    local r = doc:query('#w'):rect()
    check('set_panel_rect sizes it', r and math.abs(r.w - 320) < 1.5 and math.abs(r.h - 240) < 1.5, r and ('%s x %s'):format(r.w, r.h))
    ui.set_panel_rect('rmltest-itemscroll:w', 80, 60, 50, 50)
    gear.wait(0.7)
    r = doc:query('#w'):rect()
    check('the minimum holds', r and math.abs(r.w - 200) < 1.5 and math.abs(r.h - 150) < 1.5, r and ('%s x %s'):format(r.w, r.h))
    ui.reset_panel('rmltest-itemscroll:w')
    check('scroll("start") goes back', (list:scroll()) == 0)
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
