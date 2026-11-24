-- Pinpointed in-game checks of the text fields' clipboard and selection (docs/Gear.md, "Text fields: selection and clipboard"):
--   Sacred.exe -gear-auto -gear-load=1 -gear-mods=sacred-theme,clipboardtest -gear-instance=clip     (themed)
--   Sacred.exe -gear-auto -gear-load=1 -gear-mods=clipboardtest -gear-instance=clip                  (Gear's base style)
-- Screenshots go to GearAutomation\<instance>\; the result is the RESULT line in SacredGear.log.
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or ''))
end

local doc, shown = nil, true
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err
        doc, err = ui.rml.load('ui/field.rml')
        if not doc then print('load failed: ' .. tostring(err)) end
    end
    if doc and shown then doc:draw() end
end)

local TEXT = 'alpha beta gamma delta'
local function px(sel, fx, dx)
    local r = doc:query(sel):rect()
    return A.ui_to_pixels(r.x + (fx or 0.5) * r.w + (dx or 0), r.y + r.h / 2)
end
local function click(sel, fx, dx)
    local x, y = px(sel, fx, dx)
    A.click(x, y)
    gear.wait(0.25)
end
local function chord(key)       -- Ctrl + key
    A.key_down('control'); gear.wait(0.1)
    A.key_down(key); gear.wait(0.12)            -- held for a few frames: a poll between two frames must see it
    A.key_up(key); gear.wait(0.1)
    A.key_up('control'); gear.wait(0.15)
end
local function tap(key) A.key_down(key); gear.wait(0.12); A.key_up(key); gear.wait(0.1) end
local function shifted(key, times)
    A.key_down('shift'); gear.wait(0.08)
    for _ = 1, times or 1 do A.key_down(key); gear.wait(0.12); A.key_up(key); gear.wait(0.1) end
    A.key_up('shift'); gear.wait(0.15)
end
local function reset(sel, text)
    doc:value(sel, text); gear.wait(0.2)
end
local function copied()
    gear.clipboard.set('<unset>')
    chord('c')
    return gear.clipboard.get()
end

local function run()
    check('document loaded', doc ~= nil)
    gear.wait(0.5)
    A.screenshot('clip-1-start')

    -- Ctrl+A, Ctrl+C
    click('#f')
    chord('a')
    A.screenshot('clip-2-select-all')
    check('Ctrl+A then Ctrl+C copies the whole text', copied() == TEXT, gear.clipboard.get())
    -- Ctrl+X
    chord('x')
    check('Ctrl+X empties the field', doc:value('#f') == '', doc:value('#f'))
    check('Ctrl+X puts the text on the clipboard', gear.clipboard.get() == TEXT, gear.clipboard.get())
    -- Ctrl+V
    chord('v')
    check('Ctrl+V pastes it back', doc:value('#f') == TEXT, doc:value('#f'))
    -- typing replaces a selection
    chord('a'); A.type('Z'); gear.wait(0.2)
    check('typing over a selection replaces it', doc:value('#f') == 'Z', doc:value('#f'))
    -- paste with a line break (single line field)
    gear.clipboard.set('one\ntwo')
    reset('#f', ''); click('#f'); chord('v')
    check('paste keeps a single-line field on one line', not tostring(doc:value('#f')):find('\n', 1, true), doc:value('#f'))

    -- Shift + arrows, Shift + Home / End
    reset('#f', TEXT); click('#f'); tap('end')
    shifted('left', 5)
    A.screenshot('clip-3-shift-left')
    check('Shift+Left x5 selects the last five characters', copied() == 'delta', gear.clipboard.get())
    tap('end')
    shifted('home')
    check('Shift+Home selects to the start', copied() == TEXT, gear.clipboard.get())
    tap('home')
    shifted('end')
    check('Shift+End selects to the end', copied() == TEXT, gear.clipboard.get())
    tap('home')
    shifted('right', 5)
    check('Shift+Right x5 selects the first five characters', copied() == 'alpha', gear.clipboard.get())

    -- mouse: click-drag
    reset('#f', TEXT); gear.wait(0.2)
    do
        local x1, y1 = px('#f', 0, 1)
        local x2, y2 = px('#f', 0, 110)
        A.move(x1, y1); gear.wait(0.3); A.down(); gear.wait(0.25)
        for i = 1, 6 do A.move(x1 + (x2 - x1) * i / 6, y1); gear.wait(0.05) end
        gear.wait(0.15)
        A.screenshot('clip-4-drag')
        A.up(); gear.wait(0.2)
        local c = copied()
        check('dragging selects a prefix of the text', type(c) == 'string' and #c >= 3 and TEXT:sub(1, #c) == c, c)
    end
    -- mouse: double click on a word
    reset('#f', TEXT); gear.wait(0.2)
    do
        local x, y = px('#f', 0, 70)       -- inside the first or second word
        A.move(x, y); gear.wait(0.15)
        A.click(x, y); gear.wait(0.06); A.click(x, y); gear.wait(0.25)
        A.screenshot('clip-5-doubleclick')
        local c = copied()
        check('double click selects one word', type(c) == 'string' and #c >= 3 and not c:find(' ', 1, true) and TEXT:find(c, 1, true) ~= nil, c)
    end
    -- the other field is independent
    click('#g'); gear.wait(0.3); chord('a')
    check('second field: Ctrl+A / Ctrl+C', copied() == 'second field', gear.clipboard.get())
    A.screenshot('clip-6-second-field')

    -- the game's own text boxes (the savegame name): Ctrl+V pastes like the Paste button
    doc:query('#g'):focus(false); gear.wait(0.4)       -- a focused field of a document that is not drawn any more would keep the keyboard captured
    shown = false; gear.wait(0.4)
    gear.clipboard.set('pasted with Ctrl+V')
    for _, name in ipairs({'UI_WND_SAVEGAME'}) do
        A.show_window(name, true); gear.wait(0.8)
        A.click(700, 381); gear.wait(0.5)       -- the name box of the first slot
        A.screenshot('clip-7a-' .. name)
        chord('v'); gear.wait(0.5)
        A.screenshot('clip-7b-' .. name .. '-pasted')
        A.type('abc'); gear.wait(0.4)
        A.screenshot('clip-7c-typed')
        A.click(940, 381); gear.wait(0.6)
        A.screenshot('clip-7d-button')
        A.show_window(name, false); gear.wait(0.4)
    end
    A.press('escape'); gear.wait(0.3)
end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    run()
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
