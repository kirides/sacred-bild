-- Sacred.exe -gear-auto -gear-load=1 -gear-mods=theme,rmltest-modsbutton   (RmlUi button)   or   -gear-mods=rmltest-modsbutton   (the old button)
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local function manager_open()
    for _, p in ipairs(ui.panels()) do if p.key:match('^gear%-mods:') or p.key:match('mods:main') then return true end end
    return false
end
gear.task(function()
    if A.wait_for_menu(60) then
        gear.wait(2.5)
        A.screenshot('mods-menu')
    end
    if not A.wait_for_game(20) then return A.quit(0) end
    gear.wait(2.5)
    A.move(1500, 850); gear.wait(0.3)
    A.press('escape'); gear.wait(1.5)
    A.screenshot('mods-esc')
    A.move(A.ui_to_pixels(512, 515)); gear.wait(0.7)
    A.screenshot('mods-esc-hover')
    print('manager open before the click', manager_open())
    A.click(A.ui_to_pixels(512, 515)); gear.wait(1.5)
    A.screenshot('mods-esc-clicked')
    print('manager open after the click', manager_open())
    A.quit(0)
end)
