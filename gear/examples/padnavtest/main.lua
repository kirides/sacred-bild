-- Self test of the controller path of the mod UI (no pad needed: gear.auto.pad posts the events SacredBild's
-- controller would). Checks that SacredBild holds the widgets as stops, that the mod menu button opens the mod menu
-- (modal: only its stops), lists this mod's key and menu item and runs them, that B closes it and a closable panel,
-- and that D-pad left / right on a slider nudges it.
--
-- Run:  Sacred.exe -gear-auto -gear-load=1 -gear-instance=padnav-1 -gear-timeout=120 -gear-mods=padnavtest
-- Result: GearAutomation\padnav-1\padnavtest.json

local A = gear.auto
local ui = gear.ui

local results = {passed = 0, failed = 0, checks = {}}
local function check(name, cond, detail)
    local ok = cond and true or false
    results.passed = results.passed + (ok and 1 or 0)
    results.failed = results.failed + (ok and 0 or 1)
    results.checks[#results.checks + 1] = {name = name, ok = ok, detail = detail and tostring(detail) or nil}
    print(('%s: %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (' (' .. tostring(detail) .. ')') or ''))
end

local hello, item = 0, 0
gear.keys.on('hello', function() hello = hello + 1 end)
ui.menu_item('item', 'Test item', function() item = item + 1 end)

local value = 50
local show = false
gear.on('draw', function(state)
    if not show or state.mode ~= 'game' then return end
    local p = ui.panel('win', 'Pad test', 200, 200, 300, 120)
    if not p.open then return end
    ui.button('b1', p.content_x, p.content_y, 100, 22, 'One')
    local _, v = ui.slider('s', p.content_x, p.content_y + 40, 200, 0, 100, value, {step = 1})
    value = v
end)

local function menu_panel()
    for _, p in ipairs(ui.panels()) do
        if p.key == 'gear-mods:padmenu' then return p end
    end
end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then
        check('game with a hero', false)
        return A.quit(1)
    end
    gear.wait(2)
    local n, flags = A.nav()
    check('SacredBild takes the stops', n >= 0, n)

    -- The mod menu button.
    A.pad(0, 0, 1)
    gear.wait(0.5)
    local mp = menu_panel()
    n, flags = A.nav()
    check('mod menu opens', mp ~= nil)
    check('it is modal', flags % 2 == 1 and n >= 3, n .. ' stops, flags ' .. flags)
    A.screenshot('padnav-1-menu')
    -- Entries: Mod manager, then this mod's key (Say hello), then its item (rows of 30 from content_y).
    if mp then
        A.ui_click(mp.x + mp.w / 2, mp.y + 28 + 30 + 13)
        gear.wait(0.4)
        check('the key entry runs the key handler', hello == 1, hello)
        check('the menu closes after it', menu_panel() == nil)
    end
    A.pad(0, 0, 1)
    gear.wait(0.5)
    mp = menu_panel()
    if mp then
        A.ui_click(mp.x + mp.w / 2, mp.y + 28 + 60 + 13)
        gear.wait(0.4)
        check('the menu item runs', item == 1, item)
    end
    A.pad(0, 0, 1)
    gear.wait(0.4)
    A.pad(0, 1, 0)
    gear.wait(0.4)
    n, flags = A.nav()
    check('B closes the mod menu', menu_panel() == nil and flags % 2 == 0, flags)

    -- A window of a mod: not modal, but it takes the pad (flag 2); slider nudges; B closes it.
    show = true
    gear.wait(0.4)
    n, flags = A.nav()
    check('an open panel is a window', flags == 2, flags)
    local sx = 208 + 5 + 190 * 0.5
    local px, py = A.ui_to_pixels(sx, 200 + 28 + 40 + 8)
    A.move(px, py)
    gear.wait(0.3)
    A.pad(1, 0, 0)
    gear.wait(0.3)
    check('D-pad right nudges the slider', value == 52, value)
    A.pad(-3, 0, 0)
    gear.wait(0.3)
    check('D-pad left nudges it back', value == 46, value)
    A.screenshot('padnav-2-window')
    A.pad(0, 1, 0)
    gear.wait(0.4)
    n, flags = A.nav()
    check('B closes the panel', not ui.panel_open('win') and flags == 0, flags)
    check('and does not open the escape menu', not ui.state().escape_menu)

    A.write('padnavtest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    A.quit(results.failed == 0 and 0 or 1)
end)
