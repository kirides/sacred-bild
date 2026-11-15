-- Self test of the mod manager screen (gear/lua/modmanager.lua): opens it with its hotkey and from the escape menu,
-- pages, switches a mod off and on, changes every kind of setting with clicks, rebinds a key (and makes a conflict),
-- takes screenshots and quits. It reads where the screen's widgets are from the screen's own layout (the panel's
-- place from gear.ui.panels()), so it is tied to modmanager.lua's metrics.
--
-- Run isolated:  Sacred.exe -gear-auto -gear-load=1 -gear-instance=modmgr-1 -gear-timeout=140
--                  -gear-mods=modmanagertest,mmtest-aux,mmtest-dep
-- gear_mods.json of the run is written to GearAutomation\modmgr-1\ (not the player's), the result to
-- GearAutomation\modmgr-1\modmanagertest.json.

local A = gear.auto
local ui = gear.ui
local S = gear.settings
local cfg = gear.config

local results = {passed = 0, failed = 0, checks = {}}
local function check(name, cond, detail)
    local ok = cond and true or false
    results.passed = results.passed + (ok and 1 or 0)
    results.failed = results.failed + (ok and 0 or 1)
    results.checks[#results.checks + 1] = {name = name, ok = ok, detail = detail and tostring(detail) or nil}
    print(('%s: %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (' (' .. tostring(detail) .. ')') or ''))
end

local pinged = 0
gear.keys.on('ping', function() pinged = pinged + 1 end)

-- Where things are, from the screen's panel (modmanager.lua's metrics).
local function panel()
    for _, p in ipairs(ui.panels()) do
        if p.key == 'gear-mods:main' then return p end
    end
end
local function geo()
    local p = panel()
    if not p then return nil end
    return {p = p, x = p.x + 8, y = p.y + 28, w = p.w - 16}
end
local function click(x, y, wait)
    A.ui_click(x, y)
    gear.wait(wait or 0.4)
end

-- The row of a mod in the list: page and place on it (6 per page, sorted by name, the manager itself last).
local function row_of(id)
    local n = 0
    for _, m in ipairs(gear.mods.list()) do
        if not m.builtin then
            n = n + 1
            if m.id == id then return math.ceil(n / 6), (n - 1) % 6 + 1, n end
        end
    end
end
local function list_page(page)
    local g = geo()
    local fy = g.p.y + g.p.h - 34
    local mid = g.x + g.w / 2
    for _ = 1, 6 do click(mid - 75, fy + 11, 0.15) end            -- back to page 1
    for _ = 2, page do click(mid + 75, fy + 11, 0.25) end
end
local function row_y(slot) return geo().y + 24 + (slot - 1) * 62 end

local function open_list()
    local g = geo()
    if g then return g end
    A.press('f11')
    gear.wait(0.6)
    return geo()
end

local function listing(id) return gear.mods.get(id) end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then
        check('game with a hero', false)
        return A.quit(1)
    end
    gear.wait(3)
    A.screenshot('mm-0-game')
    check('screen starts closed', panel() == nil)

    -- The hotkey.
    A.press('f11')
    gear.wait(0.6)
    check('F11 opens the Mods screen', panel() ~= nil)
    -- The keyboard does not reach the game while the screen is open: I would open the inventory.
    A.press('i')
    gear.wait(0.8)
    local inv = ui.window('UI_WND_INVENTORY')
    check('a game hotkey (I) does nothing while the screen is open', not (inv and inv.drawn), inv and inv.drawn)
    A.screenshot('mm-1-list')
    local g = geo()
    list_page(2)
    A.screenshot('mm-2-list-page2')
    list_page(1)

    -- Switch the helper off and on from its row; the mod that requires it goes with it.
    local page, slot = row_of('mmtest-aux')
    check('the helper is listed', page ~= nil)
    check('helper and dependent run', listing('mmtest-aux').loaded and listing('mmtest-dep').loaded)
    list_page(page)
    A.screenshot('mm-3-before-switch')
    click(g.x + 8 + 22, row_y(slot) + 8 + 10, 0.8)
    check('switch the helper off: unloaded', listing('mmtest-aux').loaded == false and listing('mmtest-aux').state == 'off',
        listing('mmtest-aux').state)
    check('the dependent is off with it', listing('mmtest-dep').loaded == false and listing('mmtest-dep').state == 'blocked',
        listing('mmtest-dep').state)
    A.screenshot('mm-3-helper-off')
    click(g.x + 8 + 22, row_y(slot) + 8 + 10, 0.8)
    check('switch the helper on: loaded again', listing('mmtest-aux').loaded == true, listing('mmtest-aux').state)
    check('the dependent is back', listing('mmtest-dep').loaded == true, listing('mmtest-dep').state)

    -- This mod's own settings page.
    page, slot = row_of('modmanagertest')
    list_page(page)
    click(g.x + g.w - 176 + 40, row_y(slot) + 8 + 11, 0.5)
    A.screenshot('mm-4-settings')
    local y0 = g.y + 34
    local function ry(i) return y0 + (i - 1) * 32 end
    local cx = g.x + 290
    click(cx + 7, ry(1) + 4 + 8, 0.5)                         -- the checkbox
    check('checkbox changes the setting', S.get('flag') == true, S.get('flag'))
    click(cx + 5 + 190 * 0.8, ry(2) + 4 + 8, 0.5)             -- the slider at 80 %
    check('slider changes the setting', S.get('level') >= 7 and S.get('level') <= 9, S.get('level'))
    click(cx + 20, ry(3) + 3 + 9, 0.5)                        -- open the dropdown
    A.screenshot('mm-5-dropdown')
    click(cx + 20, ry(3) + 3 + 18 + 1 + 18 + 9, 0.5)          -- its second entry
    check('dropdown changes the setting', S.get('mode') == 'second', S.get('mode'))
    click(cx + 20, ry(4) + 2 + 10, 0.4)                       -- the text field
    for _, k in ipairs({'x', 'y', 'z'}) do A.press(k) end
    gear.wait(0.4)
    check('typing changes the text', S.get('name') == 'abcxyz', S.get('name'))
    click(cx + 30 + 62 + 27, ry(5) + 4 + 8, 0.5)              -- the red channel
    check('color slider changes the color', S.get('tint') ~= 0xFF3080C0, string.format('%08X', S.get('tint')))
    A.screenshot('mm-6-settings-changed')
    -- Reset to defaults (the button at the left of the footer).
    local fy = g.p.y + g.p.h - 34
    click(g.x + 60, fy + 11, 0.5)
    check('Reset to defaults', S.get('flag') == false and S.get('level') == 3 and S.get('mode') == 'first'
        and S.get('name') == 'abc' and S.get('tint') == 0xFF3080C0)
    click(cx + 7, ry(1) + 4 + 8, 0.4)                         -- tick the checkbox again: it must be saved

    -- Keys.
    local tabs_x = g.x + g.w - 2 * 84                         -- two tabs: Settings, Keys
    click(tabs_x + 84 + 40, g.y + 11, 0.5)
    A.screenshot('mm-7-keys')
    local ky = g.y + 34
    click(g.x + 290 + 75, ky + 1 + 11, 0.4)                   -- the Ping binding: capture
    A.screenshot('mm-8-capture')
    A.key_down('control')
    gear.wait(0.15)
    A.press('j')
    A.key_up('control')
    gear.wait(0.5)
    check('rebind to ctrl+j', gear.keys.binding('ping') == 'ctrl+j', gear.keys.binding('ping'))
    -- A key another mod uses (the helper's ctrl+f3): a conflict.
    click(g.x + 290 + 75, ky + 1 + 11, 0.4)
    A.key_down('control')
    gear.wait(0.15)
    A.press('f3')
    A.key_up('control')
    gear.wait(0.5)
    check('rebind to ctrl+f3 (the helper key)', gear.keys.binding('ping') == 'ctrl+f3', gear.keys.binding('ping'))
    A.screenshot('mm-9-conflict')
    -- Back to the list: the conflict shows there too.
    click(g.x + 35, g.y + 11, 0.5)
    list_page(page)
    A.screenshot('mm-10-list-conflict')
    -- Rebind to ctrl+j again via the Keys button and use it with the screen closed.
    click(g.x + g.w - 90 + 40, row_y(slot) + 8 + 11, 0.5)
    click(g.x + 290 + 75, ky + 1 + 11, 0.4)
    A.key_down('control')
    gear.wait(0.15)
    A.press('j')
    A.key_up('control')
    gear.wait(0.5)
    check('rebind to ctrl+j again', gear.keys.binding('ping') == 'ctrl+j', gear.keys.binding('ping'))

    -- Close with Escape; the bound key reaches the mod now; the default key does not.
    A.press('escape')
    gear.wait(0.6)
    check('Escape closes the screen', panel() == nil)
    A.screenshot('mm-after-esc')
    check('and does not open the game\'s escape menu', not ui.state().escape_menu)
    -- With the screen closed the game gets its keys again.
    A.press('i')
    gear.wait(0.8)
    inv = ui.window('UI_WND_INVENTORY')
    check('a game hotkey (I) works again once it is closed', inv and inv.drawn == true, inv and inv.drawn)
    A.press('i')
    gear.wait(0.5)
    local before = pinged
    A.key_down('control'); gear.wait(0.1); A.press('j'); A.key_up('control')
    gear.wait(0.4)
    check('the rebound key fires the mod handler', pinged == before + 1, pinged - before)
    before = pinged
    A.key_down('control'); gear.wait(0.1); A.press('f2'); A.key_up('control')
    gear.wait(0.4)
    check('the old default key does not', pinged == before, pinged - before)

    -- The escape menu has its button.
    A.press('escape')
    gear.wait(1)
    A.screenshot('mm-11-escape-menu')
    local s = ui.state()
    check('escape menu is up', s.escape_menu == true)
    click(512, 508 + 13, 0.8)
    check('the Mods button beside the escape menu opens the screen', panel() ~= nil)
    A.screenshot('mm-12-from-escape-menu')
    A.press('f11')
    gear.wait(0.5)
    check('F11 closes it again', panel() == nil)
    A.press('escape')
    gear.wait(0.5)

    -- Make sure the file is written before the run ends.
    gear.wait(1)
    A.write('modmanagertest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end)
