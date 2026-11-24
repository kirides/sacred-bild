-- gear.kit demo: Sacred.exe -gear-mods=rmlkit (plain base style) or -gear-mods=sacred-theme,rmlkit (the game's look).
local kit, ui, A = gear.kit, gear.ui, gear.auto
local doc, dlg
local tab, checked, volume = 'one', true, 6

local function markup()
    local body
    if tab == 'one' then
        body = kit.heading('Settings')
            .. kit.check('c1', 'Show the hit numbers', checked)
            .. kit.row('Volume', kit.slider('vol', 0, 10, 1, volume))
            .. kit.row('Experience', kit.progress(0.6, {class = 'xp'}))
    else
        body = kit.list({{id = 'i1', name = 'First entry', sub = 'with a smaller line', selected = true}, {id = 'i2', name = 'Second entry'}})
    end
    return kit.page(kit.window{id = 'win', title = 'Widget kit', panel = 'main', x = 120, y = 100, width = 420, close = 'x',
        body = kit.tabs{id = 'tab', tabs = {{id = 'one', label = 'Settings'}, {id = 'two', label = 'List'}}, active = tab}
            .. body .. kit.text('Everything here is plain markup from gear.kit.', {class = 'muted'})
            .. kit.buttons(kit.button('ask', 'Ask me'), kit.button('ok', 'Done', {primary = true}))})
end

local function show()
    if doc then doc:close() end
    doc = ui.rml.load_string(markup(), {})
    if not doc then return end
    doc:on('#tab-one', 'click', function() tab = 'one'; show() end)
    doc:on('#tab-two', 'click', function() tab = 'two'; show() end)
    doc:on('#c1', 'change', function() checked = not checked end)
    doc:on('#vol', 'change', function(e) volume = tonumber(e.params.value) or volume end)
    doc:on('#ask', 'click', function() dlg = kit.confirm{title = 'Really?', text = 'Do you want to go on?', yes = 'Yes', no = 'No', on_result = function(yes) gear.log('RESULT ' .. tostring(yes)) end} end)
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then show() end
    if doc then doc:draw({layer = 'top'}) end
    if dlg then if dlg:open() then dlg:draw() else dlg = nil end end
end)

if A then
    gear.task(function()
        if not A.wait_for_game(180) then return A.quit(1) end
        gear.wait(3)
        A.screenshot('kit-1-settings')
        tab = 'two'; show(); gear.wait(1)
        A.screenshot('kit-2-list')
        tab = 'one'; show(); gear.wait(0.5)
        dlg = kit.confirm{title = 'Really?', text = 'Do you want to go on?', yes = 'Yes', no = 'No', on_result = function(yes) gear.log('RESULT ' .. tostring(yes)) end}
        gear.wait(1)
        A.screenshot('kit-3-dialog')
        A.quit(0)
    end)
end
