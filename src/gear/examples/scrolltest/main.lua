-- A pinpointed self test of the scroll area and the dropdown's list (Sacred.exe -gear-auto -gear-load=1
-- -gear-mods=scrolltest): a scroll area of 100 lines of 20 (the wheel scrolls it, a line scrolled out of sight takes
-- no click, the thumb drags it), and a dropdown of 30 entries whose open list lies over text and a button drawn after
-- it on the same layer (a click on an entry selects it, nothing below reacts; the wheel scrolls the list).
local ui = gear.ui

local T = {clicked = {}, beneath = 0, under = 0, index = 1, offset = 0, max = 0}
local ITEMS = {}
for i = 1, 30 do ITEMS[i] = 'Entry ' .. i end
local AX, AY, AW, AH = 300, 150, 300, 200   -- the scroll area
local DX, DY, DW = 700, 150, 200            -- the dropdown

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    -- Below the area, drawn first: where line 12 would lie if it were not clipped.
    if ui.button('beneath', AX, AY + AH + 10, AW, 20, 'Beneath the area') then T.beneath = T.beneath + 1 end
    local area = ui.scroll_begin('area', AX, AY, AW, AH, {step = 20})
    for i = 1, 100 do
        if ui.button('line' .. i, AX + 2, area.content_y + (i - 1) * 20, area.w - 4, 18, 'Line ' .. i) then
            T.clicked[#T.clicked + 1] = i
        end
    end
    T.offset, T.max = ui.scroll_end(100 * 20)
    -- The dropdown, then text and a button on the same (top) layer where its list opens.
    local _, idx = ui.dropdown('dd', DX, DY, DW, ITEMS, T.index, {layer = 'top'})
    T.index = idx
    for i = 0, 10 do
        ui.text('Text drawn after the dropdown, over its list', DX - 20, DY + 24 + i * 20, {layer = 'top', color = 0xFFFF6060})
    end
    if ui.button('under', DX + 2, DY + 90, DW - 4, 18, 'A button under the list', {layer = 'top'}) then
        T.under = T.under + 1
    end
end)

if not gear.auto then return end
local A = gear.auto
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function drag(x0, y0, x1, y1)
    local px, py = A.ui_to_pixels(x0, y0)
    A.move(px, py); gear.wait(0.15); A.down(); gear.wait(0.2)
    for i = 1, 6 do
        local mx, my = A.ui_to_pixels(x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6)
        A.move(mx, my); gear.wait(0.1)
    end
    A.up(); gear.wait(0.4)
end

gear.task(function()
    if not A.wait_for_game(180) then
        check('game', false)
        return A.quit(1)
    end
    gear.wait(2)
    -- 1. Clipped content takes no click: line 12 would lie under the "beneath" button.
    A.ui_click(AX + 150, AY + AH + 20)
    gear.wait(0.5)
    check('a click below the area goes to what lies there', T.beneath == 1 and #T.clicked == 0,
        ('beneath %d, lines clicked %s'):format(T.beneath, table.concat(T.clicked, ',')))
    -- 2. The wheel: three notches towards the player scroll 3 x 3 lines of 20.
    A.move(A.ui_to_pixels(AX + 100, AY + 100))
    gear.wait(0.3)
    A.wheel(-3)
    gear.wait(0.6)
    check('three wheel notches scroll 180', math.abs(T.offset - 180) < 0.01, T.offset)
    -- and the line now under the cursor is the one clicked
    A.ui_click(AX + 100, AY + 5)
    gear.wait(0.4)
    check('a click hits the scrolled line', T.clicked[#T.clicked] == 10, T.clicked[#T.clicked])
    A.screenshot('scroll-area')
    -- 3. The thumb: dragged to the bottom of the track.
    local bx, bw = AX + AW - 12 + 1, 11
    local track_y, track_h = AY + bw, AH - 2 * bw
    local size, pos = 17.8, (178 - 17.8) * T.offset / T.max
    drag(bx + bw / 2, track_y + pos + size / 2, bx + bw / 2, track_y + track_h + 30)
    check('the thumb dragged to the end', math.abs(T.offset - T.max) < 0.5 and T.max == 1800, ('%s of %s'):format(T.offset, T.max))
    -- 4. The dropdown's list over the text drawn after it.
    A.ui_click(DX + 50, DY + 9)
    gear.wait(0.5)
    A.screenshot('scroll-dropdown-open')
    A.ui_click(DX + 100, DY + 18 + 1 + 4 * 18 + 9)     -- entry 5, over the "under" button
    gear.wait(0.5)
    check('a click on the list selects the entry', T.index == 5, T.index)
    check('the button under the list did not react', T.under == 0, T.under)
    -- 5. The open list scrolls with the wheel.
    A.ui_click(DX + 50, DY + 9)
    gear.wait(0.5)
    A.move(A.ui_to_pixels(DX + 100, DY + 60))
    gear.wait(0.3)
    local before = ui.scroll_offset('dd#list')
    A.wheel(-1)
    gear.wait(0.5)
    local after = ui.scroll_offset('dd#list')
    check('the wheel scrolls the open list', after - before == 54, ('%s -> %s'):format(before, after))
    A.screenshot('scroll-dropdown-scrolled')
    print(('%d passed, %d failed'):format(passed, failed))
    if gear.config.quit then A.quit(failed == 0 and 0 or 1) end
end)
