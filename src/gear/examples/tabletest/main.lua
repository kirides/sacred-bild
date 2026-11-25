-- gear.ui.table, as a mod window would use it and as a self test (Sacred.exe -gear-auto -gear-load=1
-- -gear-instance=table-1 -gear-mods=tabletest): a panel with a table of label / dropdown / checkbox / slider / button
-- rows and, under it, a table of 30 rows in a scroll area. The test checks the geometry (columns share one edge in every
-- row, the widths add up) and clicks buttons, a dropdown, a checkbox, a slider and a button in the scrolled table, and
-- counts the controller's stops.
local ui = gear.ui

local PX, PY, PW, PH = 10, 100, 560, 520
local QX, QY, QW, QH = 580, 100, 430, 300
local CLASSES = {'Gladiator', 'Battle Mage', 'Wood Elf', 'Seraphim'}
local ROWS = 30

local T = {press = {}, class = {1, 2, 3}, on = {true, false, true}, vol = {30, 60, 90}, scrolled = {}, rects = {}, q = {}}

local function rect(tag, t, col)
    local x, y, w, h = t:cell(col)
    T.rects[tag] = {x = x, y = y, w = w, h = h}
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    local p = ui.panel('win', 'Table test', PX, PY, PW, PH, {closable = false, draggable = false})
    if not p.open then return end
    -- The first column takes two shares of what the fixed ones leave, the dropdown's one (at least 110).
    local t = ui.table('party', p.content_x, p.content_y, p.content_w, {
        columns = {
            {title = 'Hero', width = '2*'},
            {title = 'Class', width = '1*', min = 110},
            {title = 'Active', width = 70},
            {title = 'Volume', width = 150},
            {title = '', width = 70, align = 'right'},
        },
        stripes = true,
    })
    T.table = t
    for i = 1, 3 do
        t:row()
        t:label(1, i == 2 and 'A hero whose name is far too long for its column' or ('Hero ' .. i))
        local _, idx = t:dropdown(2, CLASSES, T.class[i])
        T.class[i] = idx
        local _, on = t:checkbox(3, 'On', T.on[i])
        T.on[i] = on
        local _, v = t:slider(4, 0, 100, T.vol[i], {step = 5})
        T.vol[i] = v
        if t:button(5, 'Go') then T.press[i] = (T.press[i] or 0) + 1 end
        if i == 1 or i == 3 then
            for c = 1, 5 do rect('r' .. i .. 'c' .. c, t, c) end
        end
    end
    t:row(30)
    t:label(1, 'A note spanning the whole row, in a taller row', {span = 5, color = ui.COLORS.text_dim})
    T.table_h = t:height()

    -- A table in a scroll area: its height is the content height.
    local sy = p.content_y + T.table_h + 14
    ui.label('scrolllabel', 'The same layout inside a scroll area:', p.content_x, sy - 2, {bold = true})
    T.area_y = sy + 18
    local area = ui.scroll_begin('rows', p.content_x, T.area_y, p.content_w, 150, {step = 22})
    local s = ui.table('rows', area.content_x, area.content_y, area.w, {
        columns = {'*', {width = 70, align = 'right'}, 60},
        spacing = {8, 2},
    })
    T.rows_table = s
    for i = 1, ROWS do
        s:row()
        s:label(1, 'Row ' .. i)
        s:label(2, ('%d gold'):format(i * 10))
        if s:button(3, 'Pick') then T.scrolled[#T.scrolled + 1] = i end
    end
    T.scroll_h = s:height()
    ui.scroll_end(s:height())

    -- A second window: an auto column, borders, a cell that spans two rows, alignments, a row that spans all columns.
    local q = ui.panel('win2', 'Table test 2', QX, QY, QW, QH, {closable = false, draggable = false})
    if not q.open then return end
    local g = ui.table('grid', q.content_x, q.content_y, q.content_w, {
        columns = {{title = 'Key', width = 'auto'}, {title = 'Value', width = '*'}, {title = 'Act', width = 80}},
        borders = true,
        spacing = {10, 6},
        row_heights = {22, 22, 44},
    })
    T.grid = g
    g:row()
    g:label(1, 'Name')
    g:label(2, 'Alice (centred)', {halign = 'center'})
    if g:button(3, 'Edit', {rowspan = 2}) then T.q.edit = (T.q.edit or 0) + 1 end
    T.q.span = {g:cell(3, 1, 2)}
    g:row()
    g:label(1, 'A longer key')
    g:label(2, 'right', {halign = 'right'})
    g:row()
    g:label(1, 'Tall')
    g:label(2, 'top', {valign = 'top'})
    T.q.place = {g:place(3, {width = 50, height = 18, halign = 'center', valign = 'bottom'})}
    T.q.cell3 = {g:cell(3)}
    if g:button(3, 'Small', {width = 50, height = 18, halign = 'center', valign = 'bottom'}) then
        T.q.small = (T.q.small or 0) + 1
    end
    g:row(30, nil, {columns = false})
    g:label(1, 'A row across all columns', {span = 3, halign = 'center', color = ui.COLORS.text_dim})
    g:finish()
    T.q.grid_h = g:height()

    -- A table without a width: as wide as its auto columns.
    local a = ui.table('shrink', q.content_x, q.content_y + g:height() + 16, nil, {
        columns = {{width = 'auto'}, {width = 'auto'}},
    })
    a:row()
    a:label(1, 'Sized by')
    a:label(2, 'its cells')
    T.q.auto_w = a:width()
end)

if not gear.auto then return end
local A = gear.auto
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

gear.task(function()
    if not A.wait_for_game(180) then
        check('game', false)
        return A.quit(1)
    end
    gear.wait(2)
    local r = T.rects
    check('the table was drawn', T.table ~= nil and r.r1c1 ~= nil)
    if not T.table then return A.quit(1) end

    -- Geometry (no clicks).
    local same = true
    for c = 1, 5 do
        local a, b = r['r1c' .. c], r['r3c' .. c]
        if a.x ~= b.x or a.w ~= b.w then same = false end
    end
    check('every row has its columns at the same x and width', same)
    local sum = 0
    for c = 1, 5 do sum = sum + r['r1c' .. c].w end
    check('the widths and the gaps add up to the table', sum + 8 * 4 == T.table.w, ('%s + 32 vs %s'):format(sum, T.table.w))
    check('columns follow each other one gap apart',
        r.r1c2.x == r.r1c1.x + r.r1c1.w + 8 and r.r1c5.x == r.r1c4.x + r.r1c4.w + 8)
    check('a fixed column is its width', r.r1c3.w == 70 and r.r1c4.w == 150, ('%s, %s'):format(r.r1c3.w, r.r1c4.w))
    check('the shares: the first column twice the second, or the second at its minimum',
        math.abs(r.r1c1.w - 2 * r.r1c2.w) <= 2 or r.r1c2.w == 110, ('%s vs %s'):format(r.r1c1.w, r.r1c2.w))
    check('rows are a row height and a gap apart', r.r3c1.y - r.r1c1.y == 2 * (22 + 4), r.r3c1.y - r.r1c1.y)
    -- header 22 + 4 gap + 1 line, three rows of 22 with gaps, the taller row of 30, no trailing gap
    check('the height counts header, rows and the taller row', T.table_h == (22 + 4 + 1) + 3 * (22 + 4) + 30,
        T.table_h)
    A.screenshot('table-layout')

    -- Clicks.
    local b = r.r1c5
    A.ui_click(b.x + b.w / 2, b.y + b.h / 2)
    gear.wait(0.5)
    check('the button of row 1 was pressed, and only that one', T.press[1] == 1 and not T.press[2] and not T.press[3],
        tostring(T.press[1]))
    b = r.r3c5
    A.ui_click(b.x + b.w / 2, b.y + b.h / 2)
    gear.wait(0.5)
    check('the button of row 3 was pressed', T.press[3] == 1, tostring(T.press[3]))
    b = r.r3c3
    A.ui_click(b.x + 8, b.y + b.h / 2)
    gear.wait(0.5)
    check('the checkbox of row 3 toggled', T.on[3] == false, tostring(T.on[3]))
    b = r.r1c2
    A.ui_click(b.x + b.w / 2, b.y + b.h / 2)
    gear.wait(0.5)
    A.screenshot('table-dropdown-open')
    A.ui_click(b.x + b.w / 2, b.y + b.h + 1 + 3 * 18 + 9)     -- the fourth entry
    gear.wait(0.5)
    check('the dropdown of row 1 changed to its fourth entry', T.class[1] == 4 and T.class[2] == 2, T.class[1])
    local sl = r.r1c4
    A.ui_click(sl.x + 5 + (sl.w - 44 - 10) * 0.5, sl.y + sl.h / 2)
    gear.wait(0.5)
    check('the slider took a click on its track', math.abs(T.vol[1] - 50) <= 10 and T.vol[2] == 60, tostring(T.vol[1]))

    -- The table in the scroll area: the wheel scrolls it, a click hits the row under the cursor.
    local s = T.rows_table
    A.move(A.ui_to_pixels(s.x + 100, T.area_y + 60))
    gear.wait(0.3)
    A.wheel(-2)
    gear.wait(0.6)
    local off = ui.scroll_offset('rows')
    check('the wheel scrolled the table in the area', off == 132, off)
    local k = 8
    local bx, bw = s:column(3)
    A.ui_click(bx + bw / 2, T.area_y + (k - 1) * 24 - off + 11)
    gear.wait(0.5)
    check('a click hit the button of the row under it', T.scrolled[#T.scrolled] == k, tostring(T.scrolled[#T.scrolled]))
    check('the content height is the table height', T.scroll_h == ROWS * 22 + (ROWS - 1) * 2, T.scroll_h)
    A.screenshot('table-scrolled')
    local n = A.nav()
    check('the widgets are stops of the controller', n >= 15, n)

    -- The second table.
    local q = T.q
    local w1 = ui.text_size('A longer key')
    local g = T.grid
    local kx, kw = g:column(1)
    check('an auto column is as wide as its widest cell', kw ~= nil and math.abs(kw - math.ceil(w1)) <= 1,
        ('%s vs %s'):format(kw, w1))
    check('a cell reaches over two rows', q.span[4] == 22 + 6 + 22, q.span[4])
    local cx, cy, cw, ch = unpack(q.cell3)
    local px, py, pw, ph = unpack(q.place)
    check('halign centre and valign bottom place a small widget', pw == 50 and ph == 18 and py + ph == cy + ch
        and px == cx + math.floor((cw - 50) / 2), ('%s,%s %sx%s in %s,%s %sx%s'):format(px, py, pw, ph, cx, cy, cw, ch))
    local sx, sy, sw, sh = unpack(q.span)
    A.ui_click(sx + sw / 2, sy + sh - 6)       -- the lower half: row 2's part of the spanning button
    gear.wait(0.5)
    check('the spanning button takes clicks in the second row too', q.edit == 1, tostring(q.edit))
    A.ui_click(px + pw / 2, py + ph / 2)
    gear.wait(0.5)
    check('a small bottom-aligned button takes its own place', q.small == 1, tostring(q.small))
    check('the height counts the rows, the gaps and the taller rows', q.grid_h == (22 + 6 + 1) + (22 + 6) + (22 + 6) + (44 + 6) + 30,
        q.grid_h)
    local aw1 = ui.text_size('Sized by')
    local aw2 = ui.text_size('its cells')
    check('a table without a width is its auto columns and the gap', math.abs(q.auto_w - (math.ceil(aw1) + 8 + math.ceil(aw2))) <= 2,
        ('%s vs %s'):format(q.auto_w, aw1 + aw2 + 8))
    A.screenshot('table-grid')

    print(('%d passed, %d failed'):format(passed, failed))
    if gear.config.quit then A.quit(failed == 0 and 0 or 1) end
end)
