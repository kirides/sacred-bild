-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=theme,nativemenu,rmltest-menu, with nativemenu switched on in the run's gear_mods.json
-- ({"mods":{"nativemenu":{"enabled":true}}}): the menu replacement draws, a click on an entry reaches the game's menu, the Mods entry works.
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local W = 'UI_WND_ESCMENU'
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end
local function open_windows()
    local names = {}
    for _, w in ipairs(ui.windows()) do if w.visible or w.open then names[#names + 1] = w.name end end
    table.sort(names)
    return table.concat(names, ',')
end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    local before = open_windows()
    A.press('escape'); gear.wait(1.5)
    check('esc opens the menu', ui.window_open(W))
    local entries = ui.menu_entries(W)
    check('the five entries of the game menu', #entries == 5, #entries)
    for i, e in ipairs(entries) do print('entry', i, ('%q'):format(e.text), e.x, e.y, e.w, e.h) end
    local n, flags = A.nav()
    check('the native menu is up (modal pad stops: 5 entries + Mods)', n == 6 and flags % 2 == 1, ('%s stops, flags %s'):format(n, flags))
    A.screenshot('menu-1-open')
    -- a click on the native Mods entry (last button): the mod manager opens
    local list = ui.menu_entries(W)
    -- the entries are laid out by the document: pixels from the screenshot (centre column, "Mods" below the rule)
    A.click(800, 524); gear.wait(1.5)
    A.screenshot('menu-2-mods')
    -- Back to the game menu, then a click on the first entry through the synthetic path (menu_click presses it as the game's own click)
    A.press('escape'); gear.wait(1)
    if not ui.window_open(W) then A.press('escape'); gear.wait(1) end
    print('windows now', open_windows(), 'menu open', ui.window_open(W))
    local ok = ui.menu_click(W, 1)
    check('menu_click(1) accepted', ok == true, ok)
    gear.wait(1.5)
    A.screenshot('menu-3-options')
    print('windows after click 1', open_windows())
    A.press('escape'); gear.wait(1)
    -- "Weiter" (continue) is the 5th entry: it closes the menu
    if not ui.window_open(W) then A.press('escape'); gear.wait(1) end
    local open_before = ui.window_open(W)
    ui.menu_click(W, 5); gear.wait(1.2)
    check('menu_click(5) (continue) closes the game menu', open_before and not ui.window_open(W), tostring(open_before) .. ' -> ' .. tostring(ui.window_open(W)))
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
