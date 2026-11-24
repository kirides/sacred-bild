-- Slider values of the Mods screen survive opening it, switching between mods' pages and rows being reused.
-- Run isolated: -gear-auto -gear-load=1 -gear-instance=sl-1 -gear-mods=slidertest,modmanagertest  with a seeded GearAutomation\sl-1\gear_mods.json
local A, ui, S = gear.auto, gear.ui, gear.settings
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then passed = passed + 1 else failed = failed + 1 end
    print(('%s: %s (%s)'):format(cond and 'PASS' or 'FAIL', name, tostring(detail)))
end
local function q() local o = {}; gear.emit('mods.query', o); return o end
local function click_id(id, wait)
    local o = q()
    local r = o.reveal and o.reveal('#' .. id)
    if not r then return false end
    gear.wait(0.2)
    r = o.rect('#' .. id)
    local x, y = r.x + r.w / 2, r.y + r.h / 2
    local px, py
    for _ = 1, 5 do px, py = A.ui_to_pixels(x, y); if px then break end gear.wait(0.5) end
    A.move(px, py); gear.wait(0.25); A.click(px, py); gear.wait(wait or 0.5)
    return true
end
local want = {a = 14, b = 65, c = 35, d = 5, e = 11}
local function verify(tag)
    local o = q()
    for k, v in pairs(want) do
        local e = o.index.settings[k]
        local shown = e and tonumber(o.attr('#' .. e.id, 'value'))
        local txt = e and o.text('#' .. e.id .. ' + .val')
        check(tag .. ': ' .. k .. ' saved', S.get(k) == v, S.get(k))
        check(tag .. ': ' .. k .. ' slider shows ' .. v, shown == v, tostring(shown) .. ' / ' .. tostring(txt))
    end
end
gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(3)
    check('seeded values', S.get('a') == 14 and S.get('b') == 65, S.get('a'))
    A.press('f11'); gear.wait(1)
    local o = q()
    click_id(o.index.mods['slidertest'].settings, 1)
    verify('open')
    A.screenshot('sl-1-open')
    click_id('back', 1)
    for i = 1, 3 do
        local o2 = q()
        local m = o2.index.mods['modmanagertest']
        if m then click_id(m.settings, 1) end
        gear.wait(0.5)
        click_id('back', 1)
        o2 = q()
        click_id(o2.index.mods['slidertest'].settings, 1)
        verify('round ' .. i)
    end
    -- a real click on a slider still changes its setting
    do
        local o3 = q()
        local e = o3.index.settings['b']
        local r = o3.reveal('#' .. e.id)
        local px, py = A.ui_to_pixels(r.x + 37 + (r.w - 77) * 0.2, r.y + r.h / 2)
        A.move(px, py); gear.wait(0.3); A.click(px, py); gear.wait(0.6)
        check('click on slider b changes it', S.get('b') < 40, S.get('b'))
        want.b = S.get('b')
        verify('after click')
    end
    A.screenshot('sl-2-after')
    A.press('f11'); gear.wait(0.5)
    A.quit(failed == 0 and 0 or 1)
    print(('DONE passed=%d failed=%d'):format(passed, failed))
end)
