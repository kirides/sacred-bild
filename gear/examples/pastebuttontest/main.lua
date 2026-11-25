-- Pinpointed in-game check of the Paste button over the game's text boxes (docs/Gear.md):
--   Sacred.exe -gear-auto -gear-load=1 -gear-mods=pastebuttontest -gear-instance=paste
-- The savegame screen's name box shows it; the quicksave box (read-only) and a box of a hidden window do not.
if not gear.auto then return end
local A = gear.auto
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or ''))
end
local function shown()
    local q = {}
    gear.emit('mods.query', q)
    return q.paste_shown == true
end
local function at(x, y) A.click(x, y); gear.wait(0.7) end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    check('no box focused in the world: no button', not shown())
    -- the game's own way: Escape, "Spielstaende", the screen's own "Zurueck" (Back)
    A.press('escape'); gear.wait(1.0)
    at(800, 405)
    A.screenshot('paste-0-screen')
    at(700, 381)                                   -- the name box of the first slot
    A.screenshot('paste-1-slot-name')
    check('editable savegame name box: button shown', shown())
    at(700, 90)                                    -- the quicksave name box
    A.screenshot('paste-2-quicksave')
    A.type('zz'); gear.wait(0.4)                   -- it takes no typing (the screenshot shows the name unchanged)
    A.screenshot('paste-2b-quicksave-typed')
    check('quicksave (read-only) box: no button', not shown())
    at(700, 381)
    check('name box again: button shown', shown())
    at(1215, 805)                                  -- Back
    A.screenshot('paste-3-back')
    check('screen closed with its Back button: no button', not shown())
    A.press('escape'); gear.wait(0.8)              -- the menu is still up: Escape closes it
    A.screenshot('paste-4-world')
    check('back in the world: no button', not shown())
    -- hidden by the script
    A.show_window('UI_WND_SAVEGAME', true); gear.wait(0.8)
    at(700, 381)
    check('window shown by script: button shown', shown())
    A.show_window('UI_WND_SAVEGAME', false); gear.wait(0.8)
    check('window hidden by script: no button', not shown())
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
