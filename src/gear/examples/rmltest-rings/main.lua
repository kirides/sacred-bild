-- gear-ring: Sacred.exe -gear-mods=theme,rmltest-rings shows two rings and a ring of dots following a model value (it sweeps 0..1);
-- with -gear-auto (and -gear-load=1) a self test sets values and takes screenshots.
local A, ui = gear.auto, gear.ui
local doc
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err
        doc, err = ui.rml.load('ui/rings.rml', {model = {v = 0.0}})
        if not doc then print('load failed ' .. tostring(err)) end
    end
    if doc then
        if not A then doc.model.v = (gear.time() % 6) / 6 end
        doc:draw({layer = 'top'})
    end
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
    A.move(1500, 850)
    for _, v in ipairs({0.0, 0.25, 0.6, 1.0}) do
        doc.model.v = v
        gear.wait(0.6)
        check(('the attribute follows the model (%.2f)'):format(v), math.abs(tonumber(doc:query('#r1'):attr('value')) - v) < 1e-4, doc:query('#r1'):attr('value'))
        A.screenshot(('rings-%d'):format(math.floor(v * 100)))
    end
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
