-- gear.kit.osk demo: Sacred.exe -gear-mods=rmlosk (plain base style) or -gear-mods=sacred-theme,rmlosk (the game's look).
-- With -gear-auto (and a savegame: -gear-load=<slot>) it clicks the keys, the way a player does, and checks the text.
local kit, ui, A = gear.kit, gear.ui, gear.auto
local doc
local name, amount = 'Ranger', '250'
local osk = kit.osk{layout = 'qwertz'}
local closed_how

local function markup()
    return kit.page(kit.window{id = 'win', title = 'On-screen keyboard', panel = 'main', x = 200, y = 120, width = 460,
        body = kit.heading('Hero')
            .. kit.row('Name', '<input type="text" id="name" class="field" value="' .. kit.esc(name) .. '" maxlength="24"/>' .. kit.ibtn('kbd-name', 'keyboard', {tip = 'On-screen keyboard'}))
            .. kit.row('Gold', '<input type="text" id="gold" class="field" value="' .. kit.esc(amount) .. '" maxlength="8"/>' .. kit.ibtn('kbd-gold', 'keyboard', {tip = 'On-screen number pad'}))
            .. kit.text('The keyboard is a small window of its own: it works with the mouse, the D-pad and A; B or Done close it.', {class = 'muted'})})
end

local function set_field(id, text)
    local e = doc and doc:query('#' .. id)
    if e then e:value(text) end
end

local function show()
    if doc then doc:close() end
    doc = ui.rml.load_string(markup(), {})
    if not doc then return end
    doc:on('#name', 'change', function(e) name = e.params.value or name end)
    doc:on('#gold', 'change', function(e) amount = e.params.value or amount end)
    doc:on('#kbd-name', 'click', function()
        osk:toggle{title = 'Name', text = name, max = 24, on_change = function(t) name = t; set_field('name', t) end,
            get = function() return name end, on_close = function(_, how) closed_how = how end}
    end)
    doc:on('#kbd-gold', 'click', function()
        osk:toggle{title = 'Gold', digits = true, text = amount, max = 8, on_change = function(t) amount = t; set_field('gold', t) end,
            get = function() return amount end, on_close = function(_, how) closed_how = how end}
    end)
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then show() end
    if doc then doc:draw({layer = 'hud'}) end
    osk:draw()
end)

if A then
    local passed, failed = 0, 0
    local function check(what, ok, detail)
        if ok then passed = passed + 1 else failed = failed + 1 end
        print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', what, detail ~= nil and (': ' .. tostring(detail)) or ''))
    end
    local function click_in(d, sel_or_el)
        local e = type(sel_or_el) == 'string' and d:query(sel_or_el) or sel_or_el
        local r = e and e:rect()
        if not r then return false end
        A.ui_click(r.x + r.w / 2, r.y + r.h / 2)
        gear.wait(0.3)
        return true
    end
    local function kdoc()                       -- the keyboard's own document
        for _, entry in ipairs(A.rml_docs('rmlosk') or {}) do
            if entry.doc:query('#kit-osk') then return entry.doc end
        end
    end
    local function key(label)                   -- clicks the letter key with that label
        local d = kdoc()
        if not d then return false end
        for _, e in ipairs(d:query_all('.osk-key')) do
            if e:text() == label then return click_in(d, e) end
        end
        return false
    end
    local function act(id) local d = kdoc() return d and click_in(d, '#kit-osk-' .. id) end
    local function type_keys(...) for _, l in ipairs({...}) do key(l) end end

    gear.task(function()
        if not A.wait_for_game(180) then return A.quit(1) end
        gear.wait(3)
        A.screenshot('osk-0-window')
        name = ''; show(); gear.wait(0.5)

        -- the text keyboard
        click_in(doc, '#kbd-name'); gear.wait(0.4)
        check('the keyboard opens', osk:is_open() and kdoc() ~= nil)
        type_keys('q', 'u')
        check('letters type', name == 'qu', name)
        act('shift'); type_keys('I')
        check('shift gives one capital', name == 'quI', name)
        key('i')
        check('and is off again', name == 'quIi', name)
        act('space'); type_keys('ü'); act('shift'); key('Ü')
        check('space, umlaut and a shifted umlaut', name == 'quIi üÜ', name)
        act('back')
        check('backspace removes a whole UTF-8 character', name == 'quIi ü', name)
        A.screenshot('osk-1-text')
        act('clear')
        check('clear empties the text', name == '')
        gear.clipboard.set('Line one\r\nline\ttwo  ')
        act('paste')
        check('paste: line breaks and tabs become blanks, the ends are trimmed', name == 'Line one line two', name)
        gear.clipboard.set(string.rep('x', 40))
        act('paste')
        check('max length 24 holds for the clipboard', #name == 24, #name)
        key('a')
        check('and for a key', #name == 24, #name)
        A.screenshot('osk-2-max')
        act('done')
        check('Done closes it and reports done', not osk:is_open() and closed_how == 'done', closed_how)

        -- the number pad
        click_in(doc, '#kbd-gold'); gear.wait(0.4)
        check('the number pad opens', osk:is_open() and kdoc() ~= nil)
        act('clear'); type_keys('4', '2', '0')
        check('digits type', amount == '420', amount)
        gear.clipboard.set('a1b 2,3\n')
        act('paste')
        check('paste keeps digits only', amount == '420123', amount)
        gear.clipboard.set('999999999')
        act('paste')
        check('max length 8 holds', #amount == 8, amount)
        A.screenshot('osk-3-digits')
        check('no letter keys on the pad', kdoc():query('#kit-osk-space') == nil and kdoc():query('#kit-osk-shift') == nil)

        -- the player types into the field: the keyboard follows (get)
        act('done')
        click_in(doc, '#kbd-name'); gear.wait(0.4)
        name = 'typed by hand'
        gear.wait(0.3)
        check('the keyboard follows the field of the mod', osk:text() == 'typed by hand', tostring(osk:is_open()) .. ' ' .. tostring(osk:text()))

        -- the controller: every key is a stop of the D-pad, the keyboard is modal, B closes it
        local count, flags = A.nav()
        check('every key is a pad stop and the keyboard is modal', count >= 40 and flags % 2 == 1, tostring(count) .. ' stops, flags ' .. tostring(flags))
        A.pad(nil, 1)
        gear.wait(0.5)
        check('B closes the keyboard', not osk:is_open() and closed_how == 'back', closed_how)
        count, flags = A.nav()
        gear.wait(0.3)

        print(('RESULT passed %d failed %d'):format(passed, failed))
        A.quit(failed == 0 and 0 or 1)
    end)
end
