-- Self test of the mod manager screen (gear/lua/modmanager.lua, gear/lua/modmanager.rml): opens it with its hotkey and from the escape menu,
-- switches a mod off and on, changes every kind of setting with clicks, rebinds a key (and makes a conflict), picks one with the on-screen keys,
-- takes screenshots and quits. It runs with or without a theme mod (the screen is an RmlUi document) and asks the screen where its elements are
-- (gear.emit('mods.query')), so it follows the layout.
--
-- Run isolated:  Sacred.exe -gear-auto -gear-load=1 -gear-instance=modmgr-1 -gear-timeout=140
--                  -gear-mods=modmanagertest,mmtest-aux,mmtest-dep   (optionally with sacred-theme)
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

-- The screen answers gear.emit('mods.query', out) with where it is, the ids of its elements and their rectangles.
local function q()
    local out = {}
    gear.emit('mods.query', out)
    return out
end
local function panel()
    for _, p in ipairs(ui.panels()) do
        if p.key == 'gear-mods:main' then return p end
    end
end
-- A click on the UI layout point: the cursor moves there first and rests a moment, so the screen's document knows what it is over when the button goes down.
local function hclick(x, y)
    local px, py
    for _ = 1, 5 do
        px, py = A.ui_to_pixels(x, y)
        if px then break end
        gear.wait(0.5)
    end
    if not px then error('no mapping from the UI layout to the window: ' .. tostring(py)) end
    A.move(px, py)
    gear.wait(0.25)
    A.click(px, py)
end
local function click_rect(r, fx, fy, wait)
    hclick(r.x + r.w * (fx or 0.5), r.y + r.h * (fy or 0.5))
    gear.wait(wait or 0.5)
end
-- Clicks the element with this id (scrolled into view first); false when there is none.
local function click_id(id, wait, fx, fy)
    local o = q()
    local r = o.reveal and o.reveal('#' .. id)
    if not r then return false end
    gear.wait(0.2)
    r = o.rect('#' .. id)
    click_rect(r, fx, fy, wait)
    return true
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
    check('screen starts closed', panel() == nil and not q().open)

    -- The hotkey.
    A.press('f11')
    gear.wait(0.8)
    check('F11 opens the Mods screen', panel() ~= nil and q().open and q().view == 'list')
    -- The keyboard does not reach the game while the screen is open: I would open the inventory.
    A.press('i')
    gear.wait(0.8)
    local inv = ui.window('UI_WND_INVENTORY')
    check('a game hotkey (I) does nothing while the screen is open', not (inv and inv.drawn), inv and inv.drawn)
    A.screenshot('mm-1-list')
    -- Layout: the footer and Close are inside the window, below the list; the window fits the screen.
    do
        local o = q()
        local win, foot, body, close = o.rect('#win'), o.rect('#foot'), o.rect('.bodybox'), o.rect('#close')
        check('the footer and Close are inside the window', win and foot and close and foot.y >= win.y and foot.y + foot.h <= win.y + win.h
            and close.x + close.w <= win.x + win.w, foot and foot.y)
        check('the list ends above the footer', body and foot and body.y + body.h <= foot.y + 1, body and body.y + body.h)
        check('the window fits the screen', win and win.y >= 0 and win.y + win.h <= select(4, ui.screen()), win and win.y)
    end
    -- The pad's D-pad in the list: a step down with the cursor on the first row scrolls the list to the next one (when there are more rows than room).
    do
        local o = q()
        local first = o.index and next(o.index.mods) and o.rect('#me1')
        local body = o.rect('.bodybox')
        if first and body and cfg.pad_list then
            local px, py
            for _ = 1, 5 do
                px, py = A.ui_to_pixels(first.x + first.w / 2, first.y + first.h / 2)
                if px then break end
                gear.wait(0.5)
            end
            A.move(px, py)
            gear.wait(0.5)
            local before = q().scroll_top()
            for _ = 1, 3 do A.pad_scroll(1); gear.wait(0.4) end
            local after = q().scroll_top()
            check('D-pad down scrolls the Mods list to the next row', after and before and after > before, tostring(before) .. ' -> ' .. tostring(after))
            for _ = 1, 8 do A.pad_scroll(-1); gear.wait(0.2) end
        end
    end
    -- Every mod has its row (the list scrolls).
    do
        local o, missing = q(), nil
        for _, m in ipairs(gear.mods.list()) do
            local e = o.index and o.index.mods[m.id]
            if not (e and o.rect('#' .. e.en)) and not m.builtin then missing = m.id end
        end
        check('every mod is listed', missing == nil, missing)
    end
    A.screenshot('mm-2-list')

    -- Switch the helper off and on from its row; the mod that requires it goes with it.
    check('the helper is listed', q().index.mods['mmtest-aux'] ~= nil)
    check('helper and dependent run', listing('mmtest-aux').loaded and listing('mmtest-dep').loaded)
    A.screenshot('mm-3-before-switch')
    click_id(q().index.mods['mmtest-aux'].en, 0.8)
    check('switch the helper off: unloaded', listing('mmtest-aux').loaded == false and listing('mmtest-aux').state == 'off',
        listing('mmtest-aux').state)
    check('the dependent is off with it', listing('mmtest-dep').loaded == false and listing('mmtest-dep').state == 'blocked',
        listing('mmtest-dep').state)
    A.screenshot('mm-3-helper-off')
    click_id(q().index.mods['mmtest-aux'].en, 0.8)
    check('switch the helper on: loaded again', listing('mmtest-aux').loaded == true, listing('mmtest-aux').state)
    check('the dependent is back', listing('mmtest-dep').loaded == true, listing('mmtest-dep').state)

    -- This mod's own settings screen.
    click_id(q().index.mods['modmanagertest'].settings, 0.8)
    check('Settings opens the mod\'s screen', q().view == 'mod' and q().mod == 'modmanagertest' and q().tab == 'settings')
    A.screenshot('mm-4-settings')
    local function setting(id) return q().index.settings[id] end
    click_id(setting('flag').id, 0.5)                                -- the check orb
    check('checkbox changes the setting', S.get('flag') == true, S.get('flag'))
    do
        local r = q().reveal('#' .. setting('level').id)
        hclick(r.x + 37 + (r.w - 77) * 0.8, r.y + r.h / 2)          -- the slider at 80 % of its groove
        gear.wait(0.5)
    end
    check('slider changes the setting', S.get('level') >= 7 and S.get('level') <= 9, S.get('level'))
    click_id(setting('mode').next_id, 0.5)                           -- the right arrow: the second choice
    check('choice arrow changes the setting', S.get('mode') == 'second', S.get('mode'))
    A.screenshot('mm-5-choice')
    click_id(setting('mode').prev_id, 0.5)
    check('and the left arrow goes back', S.get('mode') == 'first', S.get('mode'))
    do
        local r = q().reveal('#' .. setting('tint').r_id)
        hclick(r.x + 37 + (r.w - 77) * 0.9, r.y + r.h / 2)          -- the red channel at 90 % of its (short) groove
        gear.wait(0.5)
    end
    check('color slider changes the color', S.get('tint') ~= 0xFF3080C0, string.format('%08X', S.get('tint')))
    click_id(setting('name').id, 0.4)                                -- the text field
    for _, k in ipairs({'x', 'y', 'z'}) do A.press(k) end
    gear.wait(0.5)
    check('typing changes the text', S.get('name') == 'abcxyz', S.get('name'))
    A.screenshot('mm-6-settings-changed')
    -- Reset to defaults (the button at the left of the footer).
    click_id('reset_all', 0.5)
    check('Reset to defaults', S.get('flag') == false and S.get('level') == 3 and S.get('mode') == 'first'
        and S.get('name') == 'abc' and S.get('tint') == 0xFF3080C0)
    click_id(setting('flag').id, 0.4)                                -- tick the checkbox again: it must be saved

    -- Keys.
    click_id(q().index.tabs.keys, 0.6)
    check('the Keys tab shows the keys', q().tab == 'keys')
    A.screenshot('mm-7-keys')
    local function slot() return q().index.keys['ping'].slot_id end
    click_id(slot(), 0.4)                                            -- the Ping binding: capture
    check('clicking a binding starts the capture', q().capturing == true)
    A.screenshot('mm-8-capture')
    A.key_down('control')
    gear.wait(0.15)
    A.press('j')
    A.key_up('control')
    gear.wait(0.6)
    check('rebind to ctrl+j', gear.keys.binding('ping') == 'ctrl+j', gear.keys.binding('ping'))
    -- A key another mod uses (the helper's ctrl+f3): a conflict.
    click_id(slot(), 0.4)
    A.key_down('control')
    gear.wait(0.15)
    A.press('f3')
    A.key_up('control')
    gear.wait(0.6)
    check('rebind to ctrl+f3 (the helper key)', gear.keys.binding('ping') == 'ctrl+f3', gear.keys.binding('ping'))
    A.screenshot('mm-9-conflict')
    -- The on-screen keys (for a controller): the pick button, a modifier and a key.
    click_id(q().index.keys['ping'].pick_id, 0.5)
    check('the pick button opens the on-screen keys', q().osk == true)
    A.screenshot('mm-9b-osk')
    click_id('osk_ctrl', 0.4)
    click_id('o2_1', 0.6)                                            -- the q key
    check('a key picked on screen is bound with the modifier', gear.keys.binding('ping') == 'ctrl+q', gear.keys.binding('ping'))
    -- Back to the list: the conflict shows there too.
    click_id('back', 0.6)
    check('Back returns to the list', q().view == 'list')
    A.screenshot('mm-10-list-conflict')
    -- Rebind to ctrl+j again via the Keys link and use it with the screen closed.
    click_id(q().index.mods['modmanagertest'].keys, 0.6)
    click_id(slot(), 0.4)
    A.key_down('control')
    gear.wait(0.15)
    A.press('j')
    A.key_up('control')
    gear.wait(0.6)
    check('rebind to ctrl+j again', gear.keys.binding('ping') == 'ctrl+j', gear.keys.binding('ping'))

    -- Close with Escape; the bound key reaches the mod now; the default key does not.
    A.press('escape')
    gear.wait(0.8)
    check('Escape closes the screen', panel() == nil and not q().open)
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
    A.ui_click(512, 508 + 13)
    gear.wait(0.8)
    check('the Mods button beside the escape menu opens the screen', panel() ~= nil)
    A.screenshot('mm-12-from-escape-menu')
    A.press('f11')
    gear.wait(0.8)
    check('F11 closes it again', panel() == nil)
    A.press('escape')
    gear.wait(0.5)

    -- The controller's buttons (posted as the pad would): LB / RB switch the tabs of a mod's screen; Select / "-" opens the screen from the escape menu.
    A.press('escape')
    gear.wait(1)
    check('escape menu is up again', ui.state().escape_menu == true)
    A.pad(1, 0, 0)                                                   -- the controller is in use from now on: the prompts show
    gear.wait(0.8)
    A.screenshot('mm-12b-escape-menu-pad')
    A.pad_select()
    gear.wait(0.8)
    check('Select opens the Mods screen while the escape menu is up', panel() ~= nil and q().open)
    A.screenshot('mm-13-select')
    gear.emit('mods.open', {mod = 'modmanagertest', tab = 'settings'})
    gear.wait(0.6)
    A.pad_page(1)
    gear.wait(0.6)
    check('RB goes to the next tab', q().tab == 'keys', q().tab)
    A.pad_page(-1)
    gear.wait(0.6)
    check('LB goes back to the previous tab', q().tab == 'settings', q().tab)
    A.pad_page(-1)
    gear.wait(0.6)
    check('LB on the first tab stays', q().tab == 'settings', q().tab)

    A.pad(0, 1, 0)                                                   -- B: back to the list, then out
    gear.wait(0.6)
    check('B steps back to the list', q().view == 'list', q().view)
    A.pad(0, 1, 0)
    gear.wait(0.8)
    check('and B closes the screen', panel() == nil and not q().open)
    gear.wait(0.5)
    A.press('escape')
    gear.wait(0.5)

    -- Make sure the file is written before the run ends.
    gear.wait(1)
    A.write('modmanagertest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end)
