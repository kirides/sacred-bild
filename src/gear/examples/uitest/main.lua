-- A self test of the UI in game, for runs without a player:
--
--     Sacred.exe -gear-auto -gear-load=1 -gear-mods=uitest
--
-- Opens and closes the game's windows by their hotkeys (inventory pages, log book, world map, options,
-- escape menu, a message box), takes a screenshot of each state and writes GearAutomation\uitest.json: per step the UI manager's
-- state and the visible top-level windows. Whether the screenshots look right (anchored HUD, the dimmed background
-- behind the escape menu, windows drawn alone over a black screen) is for a person or Claude to judge; the checks
-- here are that each window opens and closes again.

if not gear.auto then
    print('no automation (start the game with -gear-auto): nothing to do')
    return
end

local cfg = gear.config
local A = gear.auto
local results = {steps = {}, passed = 0, failed = 0}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
    return ok
end

-- The visible top-level windows (group:index), the message box included.
local function windows()
    local out = {}
    for _, n in ipairs(A.ui()) do
        if n.parent == 0 then out[#out + 1] = ('%s:%d'):format(n.group, n.index) end
    end
    table.sort(out)
    return table.concat(out, ' ')
end

local function snap(name)
    A.screenshot('uitest-' .. name)
    local step = {name = name, state = A.state(), windows = windows()}
    results.steps[#results.steps + 1] = step
    print(('%s: %s'):format(name, step.windows))
    return step
end

local function finish()
    A.write('uitest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end

-- Opens a window with `key`, shots it, closes it with `close` (default: the same key) and checks the HUD is back.
local function toggle(name, key, close)
    local before = windows()
    A.press(key)
    gear.wait(cfg.settle)
    local open = snap(name)
    check(name .. ' opens', open.windows ~= before, open.windows)
    A.press(close or key)
    gear.wait(cfg.settle)
    local closed = snap(name .. '-closed')
    check(name .. ' closes', closed.windows == before, closed.windows)
end

gear.task(function()
    check('main menu', A.wait_for_menu(cfg.timeout_menu))
    snap('menu')
    if not check('game with a hero', A.wait_for_game(cfg.timeout_game)) then return finish() end
    gear.wait(5)
    snap('hud')

    toggle('inventory', 'i')
    toggle('inventory-page2', 'f')
    toggle('inventory-page3', 'c')
    toggle('logbook', 'l')
    toggle('worldmap', 'm')
    toggle('options', 'o', 'escape')
    toggle('escapemenu', 'escape')

    -- A message box: escape menu -> quit asks for confirmation; its dim should cover the whole screen. Cancel it.
    A.press('escape'); gear.wait(cfg.settle)
    local esc
    for _, n in ipairs(A.ui()) do
        if n.parent == 0 and n.group == 'window' and n.index == 12 then esc = n end
    end
    -- the menu lists Options, Savegames, Export, Quit, Continue; Quit is the fourth of five rows of the window
    local quit = esc and {x = esc.x + esc.width / 2, y = esc.y + esc.height * 0.69}
    if quit then
        A.ui_click(quit.x, quit.y)
        gear.wait(cfg.settle)
        local box = snap('messagebox')
        check('message box opens', box.state.dialog ~= nil and box.state.dialog ~= false and box.state.dialog ~= 0, tostring(box.state.dialog) .. ' ' .. box.windows)
        results.messagebox_nodes = A.ui()
        A.press('escape'); gear.wait(cfg.settle)
        snap('messagebox-closed')
    else
        check('escape menu has a quit entry', false, #items)
    end
    A.press('escape'); gear.wait(cfg.settle)
    snap('escape-after-box')
    A.press('escape'); gear.wait(cfg.settle)

    -- Two windows at once: inventory and log book, then Esc closes the topmost.
    A.press('i'); gear.wait(cfg.settle)
    A.press('l'); gear.wait(cfg.settle)
    snap('inventory+logbook')
    A.press('escape'); gear.wait(cfg.settle)
    A.press('escape'); gear.wait(cfg.settle)
    snap('after-two-escapes')

    finish()
end)
