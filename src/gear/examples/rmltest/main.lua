-- Pinpointed in-game checks of the RmlUi examples (see docs/Gear.md): Sacred.exe -gear-auto -gear-load=1 -gear-mods=theme,rmltest
-- Stage 3 (theme) and 4 (layout editing) here.
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local doc, n, theme_on = nil, 0, true
gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not doc then
        local err
        doc, err = ui.rml.load('ui/theme.rml', {model = {n = 0}, calls = {inc = function() n = n + 1; doc.model.n = n end}})
        if not doc then print('load failed: ' .. tostring(err)) end
    end
    if doc and theme_on then doc:draw() end
end)

local function center(sel)
    local e = doc:query(sel)
    local r = e and e:rect()
    if not r then return nil end
    return r.x + r.w / 2, r.y + r.h / 2
end
local function click(sel)
    local x, y = center(sel)
    if not x then return check('found ' .. sel, false) end
    A.ui_click(x, y)
    gear.wait(0.4)
end

local function run_stage_theme_layout()

    check('document loaded', doc ~= nil)
    A.screenshot('theme-1-base')
    -- buttons, data binding
    click('#plain'); click('#plain')
    check('data-bound counter', n == 2, n)
    -- hover and press on the link: glow
    local x, y = center('#lnk')
    local px, py = A.ui_to_pixels(x, y)
    A.move(px, py); gear.wait(0.6)
    A.screenshot('theme-2-link-hover')
    A.down(); gear.wait(0.5)
    A.screenshot('theme-3-link-active')
    A.up(); gear.wait(0.3)
    -- layout edit
    local panels = ui.panels()
    local key
    for _, p in ipairs(panels) do if p.key == 'rmltest:win' then key = p.key; print('panel', p.key, p.title, p.x, p.y, p.w, p.h, p.resizable) end end
    check('the window is a layout panel', key ~= nil)
    ui.set_edit_mode(true); gear.wait(0.5)
    A.screenshot('layout-1-edit')
    local before = doc:query('#win'):rect()
    ui.set_panel_rect('rmltest:win', before.x + 150, before.y + 120)
    gear.wait(0.6)
    local after = doc:query('#win'):rect()
    check('set_panel_rect moves the window', math.abs(after.x - before.x - 150) < 2 and math.abs(after.y - before.y - 120) < 2,
        ('%s,%s -> %s,%s'):format(before.x, before.y, after.x, after.y))
    A.screenshot('layout-2-moved')
    -- by mouse: drag the window by its title (the editor's way is the layout mod; here the panel's own frame)
    ui.set_edit_mode(false); gear.wait(0.5)
    local l = ui.layout()['rmltest:win']
    check('the place is kept in the layout', l ~= nil and l.x ~= nil, l and l.x)
    A.screenshot('layout-3-after-edit')
    ui.reset_layout('rmltest:')
    theme_on = false
end

-- ============================================================================================================
-- Stage 5: a modal document; stage 6: element handles and delegated listeners.
local modal, modal_open, modal_first = nil, false, false
local hits = {ok = 0, cancel = 0, plain_before = 0, plain_after = 0, rml_other = 0}
local dom, dom_hits = nil, {delegated = 0, parent = 0, direct = 0}
local back_seen = 0

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if not modal then
        modal = ui.rml.load_string('<rml><body><div id="dlg" class="window plain" style="position: absolute; left: 330px; top: 300px; width: 360px;">'
            .. '<p class="heading">Modal dialog</p><div class="row"><button id="ok" class="primary">OK</button><button id="cancel">Cancel</button></div></div></body></rml>',
            {dp = false})
        modal:on('#ok', 'click', function() hits.ok = hits.ok + 1 end)
        modal:on('#cancel', 'click', function() hits.cancel = hits.cancel + 1 end)
    end
    -- A plain gear.ui button drawn before and one after the modal document: both lie under the dialog's backdrop area.
    if ui.button('plain_before', 40, 700, 150, 24, 'Button before') then hits.plain_before = hits.plain_before + 1 end
    if modal_open and modal_first then modal:draw({layer = 'top', modal = true}) end
    if ui.button('plain_after', 220, 700, 150, 24, 'Button after') then hits.plain_after = hits.plain_after + 1 end
    if modal_open and not modal_first then modal:draw({layer = 'top', modal = true}) end
    if modal_open and ui.back_pressed() then back_seen = back_seen + 1 end

    if not dom then
        dom = ui.rml.load_string('<rml><body><div id="box" class="window plain" style="position: absolute; left: 700px; top: 80px; width: 260px;">'
            .. '<div id="list"><button class="t" id="t1">first</button></div><div id="sink"></div></div></body></rml>')
        dom:on('.t', 'click', function() dom_hits.delegated = dom_hits.delegated + 1 end)       -- delegated: also covers what is appended later
        dom:query('#list'):on('click', function() dom_hits.parent = dom_hits.parent + 1 end)    -- a plain listener of the parent: bubbling once
    end
    if dom then dom:draw() end
end)

local function pixel_of(doc_, sel)
    local r = doc_:query(sel):rect()
    return A.ui_to_pixels(r.x + r.w / 2, r.y + r.h / 2)
end



local function run_stage_modal()
    -- stage 5
    modal_open, modal_first = true, false
    gear.wait(1)
    local count, flags = A.nav()
    check('modal: the pad holds only the dialog\'s stops', count == 2 and flags % 2 == 1, ('stops %s flags %s'):format(count, flags))
    A.screenshot('modal-1-open')
    A.click(640, 354); gear.wait(0.4)      -- the Count button of another mod's document (rmldemo, if it runs)
    local x, y = A.ui_to_pixels(40 + 75, 700 + 12)
    A.click(x, y); gear.wait(0.4)
    local x2, y2 = A.ui_to_pixels(220 + 75, 700 + 12)
    A.click(x2, y2); gear.wait(0.4)
    check('modal: a button drawn before takes no click', hits.plain_before == 0, hits.plain_before)
    check('modal: a button drawn after takes no click', hits.plain_after == 0, hits.plain_after)
    A.click(pixel_of(modal, '#ok')); gear.wait(0.4)
    check('modal: its own button works once', hits.ok == 1, hits.ok)
    -- the pad: D-pad steps over the two stops only
    A.pad(1, 0, 0); gear.wait(0.3)
    A.pad(1, 0, 0); gear.wait(0.3)
    A.screenshot('modal-2-pad')
    A.pad(0, 1, 0); gear.wait(0.4)
    check('modal: B reaches the mod (back_pressed)', back_seen >= 1, back_seen)
    -- the other draw order
    modal_first = true; gear.wait(0.5)
    A.click(x, y); gear.wait(0.4); A.click(x2, y2); gear.wait(0.4)
    check('modal, drawn first: still inert below', hits.plain_before == 0 and hits.plain_after == 0, hits.plain_before .. '/' .. hits.plain_after)
    A.click(pixel_of(modal, '#cancel')); gear.wait(0.4)
    check('modal, drawn first: its own button works', hits.cancel == 1, hits.cancel)
    modal_open = false; gear.wait(0.5)
    local c2, f2 = A.nav()
    check('modal closed: the flag is gone', f2 % 2 == 0, ('stops %s flags %s'):format(c2, f2))
    A.click(x, y); gear.wait(0.4)
    check('modal closed: the button below works again', hits.plain_before == 1, hits.plain_before)
end

local function run_stage_dom()
    local list = dom:query('#list')
    A.click(pixel_of(dom, '#t1')); gear.wait(0.4)
    check('dom: delegated listener fires once', dom_hits.delegated == 1, dom_hits.delegated)
    check('dom: parent listener fires once (bubbling)', dom_hits.parent == 1, dom_hits.parent)
    local b2 = list:append('button', {class = 't', id = 't2'})
    check('dom: append returns a handle', b2 ~= nil)
    b2:text('second'); gear.wait(0.5)
    check('dom: query finds the appended element', dom:query('#t2') ~= nil and dom:query('#t2'):text() == 'second')
    A.click(pixel_of(dom, '#t2')); gear.wait(0.4)
    check('dom: delegated listener covers the appended element once', dom_hits.delegated == 2 and dom_hits.parent == 2, dom_hits.delegated .. '/' .. dom_hits.parent)
    local all = list:query_all('.t')
    check('dom: query_all', #all == 2, #all)
    check('dom: children / parent', #list:children() == 2 and list:parent():attr('id') == 'box')
    b2:class('hot', true); b2:attr('title', 'x'); b2:style('color', '#ff0000')
    check('dom: class / attr / style', b2:attr('title') == 'x')
    A.screenshot('dom-1-appended')
    check('dom: remove', b2:remove() == true)
    gear.wait(0.3)
    check('dom: removed handle is dead without error', b2:exists() == false and b2:text() == nil and b2:rect() == nil)
    check('dom: query no longer finds it', dom:query('#t2') == nil)
    A.click(pixel_of(dom, '#t1')); gear.wait(0.4)
    check('dom: a third click fires once more', dom_hits.delegated == 3 and dom_hits.parent == 3, dom_hits.delegated .. '/' .. dom_hits.parent)
    -- many appends: no crash, listeners still once
    for i = 1, 40 do local e = dom:query('#sink'):append('span', {class = 'x'}); e:text('n' .. i) end
    gear.wait(0.5)
    check('dom: 40 appends', #dom:query('#sink'):children() == 40)
    dom:query('#sink'):html('<span>replaced</span>'); gear.wait(0.3)
    check('dom: html replaces', #dom:query('#sink'):children() == 1)
end

gear.task(function()
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(2.5)
    run_stage_theme_layout()
    run_stage_modal()
    run_stage_dom()
    ui.reset_layout('rmltest:')
    print(('RESULT passed %d failed %d'):format(passed, failed))
    A.quit(failed == 0 and 0 or 1)
end)
