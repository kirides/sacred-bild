-- A layout editor in the manner of an MMO's edit mode: F7 (or its button) unlocks the UI. Then every open game window
-- (inventory, equipment, skills, minimap, taskbar, shops, log book ...) and every mod panel is covered by a translucent
-- blue overlay with its name, so you still see what lies below it:
--  * click a frame to select it (gold); drag it anywhere on its overlay to move it;
--  * it snaps to the screen's edges and centre and to the other frames' edges and centres (hold Shift: no snapping);
--  * the arrow keys nudge the selected frame by one unit of the virtual 4096 x 4096 screen (Shift: 10);
--  * its settings popup shows its place, resets it and, for resizable panels, sets the size (or drag the corner);
--  * a click on empty space deselects it; Done (F7) ends the edit mode.
-- Places are kept in mods\gear_layout.json in that virtual screen, so they stay when the resolution changes.
-- Moving the game's windows needs SacredBild (its UI frames); the mod panels move without it.
--
-- Self test, run twice: Sacred.exe -gear-auto -gear-load=1 -gear-mods=layout -gear-shared-input
-- The first run selects, drags, nudges and resizes, and quits; the second checks the places came back, takes
-- screenshots and resets the layout.

local cfg = gear.config
local ui = gear.ui
local C = ui.COLORS

local TOOLBAR = 'layout:toolbar'
local COLOR = {
    fill = 0x5A2A64C8, fill_hover = 0x783A78E0, edge = 0xFF6AAEFF,
    sel_fill = 0x5AD8A018, sel_hover = 0x78E8B428, sel_edge = 0xFFFFD040,
    name = 0xFFFFFFFF, sel_name = 0xFFFFE6A0, shadow = 0xD0000000, guide = 0xD0FFE070,
}
local SNAP = 8              -- layout units within which a dragged frame snaps

local selected = nil        -- the key of the selected frame ('game:UI_WND_INVENTORY', 'mod:panel')
local drag = nil            -- {key, mode = 'move' | 'size', ...} while the mouse button is held on a frame
local snapping = cfg.snap ~= false
local toolbar_rect = nil    -- the toolbar's place this frame (for the self test)
local popup_rect = nil      -- the settings popup's
local popup_pin = nil       -- where the popup was put for the selected frame's place

local NAMES = {
    UI_WND_STATS_CHAR = 'Character', UI_WND_TASKBAR = 'Action bar', UI_WND_QUESTBOOK = 'Quest log',
    UI_WND_OVERVIEWMAP = 'Minimap', UI_WND_NETPORTRAIT = 'Party', UI_WND_TEXTLINES = 'Messages',
    UI_WND_NETINFO = 'Network info', UI_WND_MASTER = 'Combat master',
}

-- 'UI_WND_SKILL_TREE' -> 'Skill Tree'
local function pretty(name)
    if NAMES[name] then return NAMES[name] end
    local s = name:gsub('^UI_WND_', ''):gsub('^UI_', ''):lower():gsub('_', ' ')
    return (s:gsub('(%a)(%w*)', function(a, b) return a:upper() .. b end))
end

-- Every frame the edit mode covers: {key, name, x, y, w, h, game | panel, resizable, min_w, min_h}. A game window's
-- frame is where it drew (its tabs and ornaments too, `bounds`), else its rect; ox, oy: from there to its place.
local function collect()
    local list = {}
    if ui.can_move_windows() then
        for _, w in ipairs(ui.windows()) do
            local b = w.bounds
            if w.drawn and w.movable and w.name ~= '' and (b or (w.width > 0 and w.height > 0)) then
                local wx, wy = w.x + w.frame_x, w.y + w.frame_y
                b = b or {x = wx, y = wy, w = w.width, h = w.height}
                list[#list + 1] = {key = 'game:' .. w.name, name = pretty(w.name), game = w.name,
                    x = b.x, y = b.y, w = b.w, h = b.h, ox = wx - b.x, oy = wy - b.y}
            end
        end
    end
    for _, p in ipairs(ui.panels()) do
        if p.key ~= TOOLBAR then
            list[#list + 1] = {key = p.key, name = p.title ~= '' and p.title or p.key, panel = true,
                x = p.x, y = p.y, w = p.w, h = p.h, resizable = p.resizable, min_w = p.min_width, min_h = p.min_height}
        end
    end
    return list
end

local function move(f, x, y, w, h)
    if f.game then
        ui.set_window_position(f.game, x + f.ox, y + f.oy)
    else
        ui.set_panel_rect(f.key, x, y, w, h)
        if f.resizable and w then
            f.w, f.h = math.max(w, f.min_w), math.max(h, f.min_h)
        end
    end
    f.x, f.y = x, y
end

local function reset(f)
    if f.game then ui.reset_window(f.game) else ui.reset_panel(f.key) end
end

-- Moves `pos` (a frame of `size`) so that its start, centre or end lies on the nearest target within SNAP.
local function snap_axis(pos, size, targets)
    local best, dist, line = pos, SNAP + 0.001, nil
    for _, t in ipairs(targets) do
        for _, edge in ipairs({0, size / 2, size}) do
            local d = math.abs(pos + edge - t)
            if d < dist then best, dist, line = t - edge, d, t end
        end
    end
    return best, line
end

local guides = {}           -- the snap lines of this frame: {vertical = x} / {horizontal = y}

local function snap(f, x, y, list, sl, st, sr, sb)
    local xs, ys = {sl, sr, (sl + sr) / 2}, {st, sb, (st + sb) / 2}
    for _, o in ipairs(list) do
        if o.key ~= f.key then
            xs[#xs + 1], xs[#xs + 2], xs[#xs + 3] = o.x, o.x + o.w, o.x + o.w / 2
            ys[#ys + 1], ys[#ys + 2], ys[#ys + 3] = o.y, o.y + o.h, o.y + o.h / 2
        end
    end
    local gx, gy
    x, gx = snap_axis(x, f.w, xs)
    y, gy = snap_axis(y, f.h, ys)
    if gx then guides[#guides + 1] = {vertical = gx} end
    if gy then guides[#guides + 1] = {horizontal = gy} end
    return x, y
end

-- One virtual unit (Shift: 10) at a time, from the stored place so nothing drifts.
local function nudge(f, dx, dy)
    local e = ui.layout()[f.key]
    local vx, vy
    if e then vx, vy = e.x, e.y else vx, vy = ui.to_virtual(f.x + (f.ox or 0), f.y + (f.oy or 0)) end
    local x, y = ui.from_virtual(vx + dx, vy + dy)
    move(f, x - (f.ox or 0), y - (f.oy or 0))
end

-- Arrow keys with auto repeat (after about 0.3 s, then every third frame).
local held = {}
local function repeating(key)
    if not ui.key_down(key) then
        held[key] = nil
        return false
    end
    local n = (held[key] or 0) + 1
    held[key] = n
    return n == 1 or (n > 18 and n % 3 == 0)
end

-- The largest of a few fonts in which the name fits the frame (cached per name and font).
local sizes = {}
local function name_font(name, max_w, max_h)
    for _, size in ipairs({30, 22, 16, 12}) do
        local font = {size = size, bold = true}
        local k = name .. '\0' .. size
        local s = sizes[k]
        if not s then
            local w, h = ui.text_size(name, font)
            s = {w or #name * 8, h or 16}
            sizes[k] = s
        end
        if s[1] <= max_w and s[2] <= max_h then return font, s[1], s[2] end
    end
end

local function overlay(f, sel, hot)
    local top = {layer = 'top'}
    local fill = sel and (hot and COLOR.sel_hover or COLOR.sel_fill) or (hot and COLOR.fill_hover or COLOR.fill)
    ui.rect(f.x, f.y, f.w, f.h, fill, top)
    ui.frame(f.x, f.y, f.w, f.h, sel and COLOR.sel_edge or COLOR.edge, sel and 3 or 1, top)
    local font, tw, th = name_font(f.name, f.w - 12, f.h - 4)
    if font then
        local tx, ty = math.floor(f.x + (f.w - tw) / 2), math.floor(f.y + (f.h - th) / 2)
        ui.text(f.name, tx, ty, {size = font.size, bold = true, outline = true,
            color = sel and COLOR.sel_name or COLOR.name, layer = 'top'})
    end
    if f.resizable then
        local c = sel and COLOR.sel_edge or COLOR.edge
        for i = 0, 2 do
            ui.rect(f.x + f.w - 5 - i * 4, f.y + f.h - 5, 2, 2, c, top)
            ui.rect(f.x + f.w - 5, f.y + f.h - 5 - i * 4, 2, 2, c, top)
        end
    end
end

local function overlaps(a, x, y, w, h)
    return a and x < a.x + a.w and a.x < x + w and y < a.y + a.h and a.y < y + h
end

-- The selected frame's settings, beside it (right, left, below, above; the first place on the screen that leaves the
-- toolbar free, else the first on the screen, else inside the frame).
local function popup(f, sl, st, sr, sb)
    local top = {layer = 'top'}
    local pw, ph = 230, f.resizable and 150 or 108
    local function clamp_y(y) return math.max(st, math.min(sb - ph, y)) end
    local function clamp_x(x) return math.max(sl, math.min(sr - pw, x)) end
    local spots = {
        {f.x + f.w + 10, clamp_y(f.y)}, {f.x - pw - 10, clamp_y(f.y)},
        {clamp_x(f.x), f.y + f.h + 10}, {clamp_x(f.x), f.y - ph - 10},
    }
    local px, py
    for pass = 1, 2 do
        for _, s in ipairs(spots) do
            if not px and s[1] >= sl and s[1] + pw <= sr and s[2] >= st and s[2] + ph <= sb
                and (pass == 2 or not overlaps(toolbar_rect, s[1], s[2], pw, ph)) then
                px, py = s[1], s[2]
            end
        end
    end
    if not px then px, py = clamp_x(f.x + 10), clamp_y(f.y + 10) end
    px, py = math.floor(px), math.floor(py)
    -- It stays put while only the frame's size changes (its own sliders would move away under the cursor).
    local pin = popup_pin
    -- (Places come back through the virtual screen: compare within a unit.)
    if pin and pin.key == f.key and math.abs(pin.fx - f.x) < 1 and math.abs(pin.fy - f.y) < 1 and pin.h == ph then
        px, py = pin.x, pin.y
    else
        popup_pin = {key = f.key, fx = f.x, fy = f.y, h = ph, x = px, y = py}
    end
    popup_rect = {x = px, y = py, w = pw, h = ph}
    ui.region('popup', px, py, pw, ph, top)
    ui.rect(px, py, pw, ph, 0xEC101018, top)
    ui.frame(px, py, pw, ph, COLOR.sel_edge, 1, top)
    ui.rect(px + 1, py + 1, pw - 2, 20, C.title, top)
    ui.text(f.name, px + 8, py + 3, {color = C.accent, layer = 'top'})
    if ui.button('popup-close', px + pw - 21, py + 2, 18, 18, 'x', {layer = 'top', tooltip = 'Deselect'}) then
        selected = nil
    end
    local y = py + 28
    local vx, vy = ui.to_virtual(f.x + (f.ox or 0), f.y + (f.oy or 0))
    ui.text(('Position %d, %d  (of 4096)'):format(math.floor(vx + 0.5), math.floor(vy + 0.5)), px + 8, y,
        {color = C.text, layer = 'top'})
    ui.text(f.key, px + 8, y + 16, {color = C.text_dim, layer = 'top'})
    y = y + 38
    if f.resizable then
        local l, t, r, b = sl, st, sr, sb
        ui.text('Width', px + 8, y, {layer = 'top'})
        local cw, nw = ui.slider('popup-w', px + 58, y, 120, f.min_w, math.floor(r - l), math.floor(f.w + 0.5),
            {layer = 'top'})
        ui.text('Height', px + 8, y + 22, {layer = 'top'})
        local ch, nh = ui.slider('popup-h', px + 58, y + 22, 120, f.min_h, math.floor(b - t), math.floor(f.h + 0.5),
            {layer = 'top'})
        if cw or ch then move(f, f.x, f.y, nw, nh) end
        y = y + 46
    end
    if ui.button('popup-reset', px + 8, y, 120, 22, 'Reset position', {layer = 'top',
        tooltip = f.resizable and 'Its own place and size again' or 'Its own place again'}) then
        reset(f)
    end
    ui.text('Arrows: nudge', px + 136, y + 4, {color = C.text_dim, layer = 'top'})
end

local function editor(state)
    local sl, st, sr, sb = state.screen_left, state.screen_top, state.screen_right, state.screen_bottom
    local top = {layer = 'top'}
    local list = collect()
    guides = {}
    popup_rect = nil

    -- Behind the overlays: the whole screen, so a click on empty space deselects (and does not walk the hero).
    local bg = ui.region('background', sl, st, sr - sl, sb - st, top)
    if bg.clicked then selected = nil end

    -- Larger frames first, so the smaller ones on top of them stay reachable; the selected one on top of all.
    table.sort(list, function(a, b)
        local sa, sb_ = a.key == selected, b.key == selected
        if sa ~= sb_ then return sb_ end
        local aa, ab = a.w * a.h, b.w * b.h
        if aa ~= ab then return aa > ab end
        return a.key < b.key
    end)
    local by_key = {}
    local mx, my = ui.mouse()
    for _, f in ipairs(list) do
        by_key[f.key] = f
        local r = ui.region('frame:' .. f.key, f.x, f.y, f.w, f.h, top)
        if r.pressed then
            selected = f.key
            drag = {key = f.key, mode = 'move', grab_x = mx - f.x, grab_y = my - f.y, start_x = mx, start_y = my}
        end
        local g = f.resizable and ui.region('grip:' .. f.key, f.x + f.w - 16, f.y + f.h - 16, 16, 16,
            {layer = 'top', tooltip = 'Drag to resize'})
        if g and g.pressed then
            selected = f.key
            drag = {key = f.key, mode = 'size', grab_w = mx - f.w, grab_h = my - f.h}
        end
        if drag and drag.key == f.key then
            if drag.mode == 'move' and r.held then
                -- A click selects; only a drag (a few units) moves.
                if drag.moved or math.abs(mx - drag.start_x) + math.abs(my - drag.start_y) >= 3 then
                    drag.moved = true
                    local x, y = mx - drag.grab_x, my - drag.grab_y
                    if snapping and not ui.key_down('shift') then x, y = snap(f, x, y, list, sl, st, sr, sb) end
                    move(f, x, y)
                end
            elseif drag.mode == 'size' and g and g.held then
                move(f, f.x, f.y, math.max(f.min_w, mx - drag.grab_w), math.max(f.min_h, my - drag.grab_h))
            else
                drag = nil
            end
        end
        overlay(f, f.key == selected, r.hover or (g and g.hover) or (drag and drag.key == f.key))
    end
    for _, gl in ipairs(guides) do
        if gl.vertical then ui.rect(gl.vertical, st, 1, sb - st, COLOR.guide, top) end
        if gl.horizontal then ui.rect(sl, gl.horizontal, sr - sl, 1, COLOR.guide, top) end
    end

    local f = selected and by_key[selected]
    if f then
        if not drag then
            local step = ui.key_down('shift') and 10 or 1
            if repeating('left') then nudge(f, -step, 0) end
            if repeating('right') then nudge(f, step, 0) end
            if repeating('up') then nudge(f, 0, -step) end
            if repeating('down') then nudge(f, 0, step) end
        end
    end
    return f
end

local function toolbar(state)
    local p = ui.panel('toolbar', 'Edit mode', 400, 300, 250, 160, {layer = 'top', closable = false,
        resizable = true, min_width = 250, min_height = 160, tooltip = 'Drag me too'})
    toolbar_rect = p.open and {x = p.x, y = p.y, w = p.w, h = p.h} or nil
    if not p.open then return end
    local x, y = p.content_x, p.content_y
    if ui.can_move_windows() then
        ui.text('Click a frame, drag it anywhere.', x, y, {layer = 'top'})
    else
        ui.text('Game windows need SacredBild.', x, y, {color = 0xFFFF8080, layer = 'top'})
    end
    ui.text('Arrows nudge it (Shift: 10).', x, y + 15, {color = C.text_dim, layer = 'top'})
    if ui.button('done', x, y + 36, 112, 22, 'Done (F7)', {layer = 'top'}) then ui.set_edit_mode(false) end
    if ui.button('reset-game', x + 122, y + 36, 112, 22, 'Reset windows', {layer = 'top',
        tooltip = 'The game windows back to their places'}) then
        ui.reset_layout('game:')
    end
    if ui.button('reset-all', x, y + 64, 234, 22, 'Reset everything', {layer = 'top',
        tooltip = 'Game windows and every mod panel'}) then
        ui.reset_layout()
    end
    local changed, on = ui.checkbox('snap', x, y + 94, 'Snap to edges and frames', snapping, {layer = 'top',
        tooltip = 'Hold Shift while dragging to place freely'})
    if changed then snapping = on end
end

-- The self test's own resizable panel, so a mod panel shows up in the edit mode without another mod.
local sample_panel = gear.auto ~= nil

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then
        drag = nil
        return
    end
    if sample_panel then
        local p = ui.panel('sample', 'Sample panel', 60, 120, 200, 110, {resizable = true, min_width = 140,
            min_height = 80, closable = false})
        if p.open then ui.text('A mod panel', p.content_x, p.content_y, {}) end
    end
    if ui.key_pressed(cfg.key) then ui.set_edit_mode(not ui.edit_mode()) end
    if ui.edit_mode() then
        -- The editor covers the screen on the top layer: out of the way of the escape menu and message boxes.
        if state.escape_menu or state.message_box then
            drag = nil
            return
        end
        local f = editor(state)
        toolbar(state)
        -- The selected frame's settings over everything, the toolbar too.
        if f then popup(f, state.screen_left, state.screen_top, state.screen_right, state.screen_bottom) end
    else
        selected, drag, toolbar_rect, popup_rect = nil, nil, nil, nil
        if ui.button('edit', state.screen_left + 4, state.screen_top + 4, 80, 20, 'Edit layout',
            {tooltip = 'Move windows and panels (F7)'}) then
            ui.set_edit_mode(true)
        end
    end
end)

---------------------------------------------------------------------------------------------------------------
-- Self test
---------------------------------------------------------------------------------------------------------------
if not gear.auto then return end
local A = gear.auto
local results = {passed = 0, failed = 0}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function drag_ui(x0, y0, x1, y1)
    local px, py = A.ui_to_pixels(x0, y0)
    A.move(px, py); gear.wait(0.15); A.down(); gear.wait(0.2)
    for i = 1, 6 do
        local mx, my = A.ui_to_pixels(x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6)
        A.move(mx, my); gear.wait(0.1)
    end
    A.up(); gear.wait(0.5)
end

local function inventory()
    local w = ui.window('UI_WND_INVENTORY')
    if w then return w, w.x + w.frame_x, w.y + w.frame_y end
end

local function sample()
    for _, p in ipairs(ui.panels()) do
        if p.key == 'layout:sample' then return p end
    end
end

-- A point on no frame, toolbar or popup.
local function empty_spot()
    local s = ui.state()
    local rects = collect()
    if toolbar_rect then rects[#rects + 1] = toolbar_rect end
    if popup_rect then rects[#rects + 1] = popup_rect end
    for y = s.screen_top + 40, s.screen_bottom - 40, 24 do
        for x = s.screen_left + 120, s.screen_right - 40, 24 do
            local free = true
            for _, r in ipairs(rects) do
                if x >= r.x - 4 and y >= r.y - 4 and x < r.x + r.w + 4 and y < r.y + r.h + 4 then free = false break end
            end
            if free then return x, y end
        end
    end
end

gear.task(function()
    check('main menu', A.wait_for_menu(120))
    if not A.wait_for_game(180) then
        check('game with a hero', false)
        return A.quit(1)
    end
    gear.wait(3)
    check('game windows can move (SacredBild)', ui.can_move_windows())
    A.press('i')
    gear.wait(1.5)
    local lay = ui.layout()
    local second = lay['layout:sample'] ~= nil and lay['game:UI_WND_INVENTORY'] ~= nil
    results.run = second and 'second' or 'first'
    print('run: ' .. results.run .. ', layout: ' .. gear.json.encode(lay))
    if not second then
        local w, x, y = inventory()
        check('inventory open', w and w.drawn)
        -- The cursor on the inventory's first item: the game shows its tooltip, but not under the edit mode.
        local hx, hy = A.ui_to_pixels(x + 18, y + 24)
        A.move(hx, hy)
        gear.wait(1.2)
        A.screenshot('hover-normal')
        A.press('f7')
        gear.wait(0.6)
        check('F7 turns the edit mode on', ui.edit_mode())
        A.move(hx + 1, hy)
        gear.wait(1.2)
        A.screenshot('editmode-hover')
        A.screenshot('editmode-on')
        for _, ww in ipairs(ui.windows()) do
            if ww.visible or ww.bounds then
                local b = ww.bounds
                print(('window %s slot %d visible %s drawn %s movable %s rect %d,%d %dx%d frame %d,%d bounds %s'):format(
                    ww.name, ww.slot, tostring(ww.visible), tostring(ww.drawn), tostring(ww.movable), ww.x, ww.y,
                    ww.width, ww.height, ww.frame_x, ww.frame_y,
                    b and ('%d,%d %dx%d'):format(b.x, b.y, b.w, b.h) or '-'))
            end
        end
        local inv
        for _, fr in ipairs(collect()) do
            print(('frame %s %q at %d,%d %dx%d'):format(fr.key, fr.name, fr.x, fr.y, fr.w, fr.h))
            if fr.key == 'game:UI_WND_INVENTORY' then inv = fr end
        end
        check('inventory overlay covers its tabs', inv and inv.y < y - 8, inv and ('overlay y %d, window y %d'):format(inv.y, y))

        -- A click selects (and moves nothing).
        A.ui_click(x + w.width / 2, y + w.height / 2)
        gear.wait(0.5)
        local _, xs, ys = inventory()
        check('click selects the inventory', selected == 'game:UI_WND_INVENTORY', selected)
        check('a click does not move it', xs and math.abs(xs - x) < 1 and math.abs(ys - y) < 1)
        A.screenshot('editmode-selected')

        -- A drag anywhere on the overlay moves it (snapping may pull it a few units).
        -- (Not over the sample panel: the selected frame lies on top of the others.)
        drag_ui(x + 300, y + 120, x + 300 + 250, y + 120 - 100)
        local _, x2, y2 = inventory()
        check('inventory dragged', x2 and math.abs(x2 - (x + 250)) <= SNAP + 1 and math.abs(y2 - (y - 100)) <= SNAP + 1,
            ('%s,%s -> %s,%s'):format(x, y, tostring(x2), tostring(y2)))
        A.screenshot('editmode-dragged')

        -- Arrow keys nudge by one virtual unit.
        local before = ui.layout()['game:UI_WND_INVENTORY']
        A.press('right'); gear.wait(0.2); A.press('down'); gear.wait(0.4)
        local after = ui.layout()['game:UI_WND_INVENTORY']
        check('arrows nudge by one unit', before and after and math.abs(after.x - before.x - 1) < 0.01
            and math.abs(after.y - before.y - 1) < 0.01,
            before and after and ('%.2f,%.2f -> %.2f,%.2f'):format(before.x, before.y, after.x, after.y))

        -- A mod panel: select it, resize it at its corner.
        local p = sample()
        check('sample panel listed', p ~= nil)
        if p then
            A.ui_click(p.x + p.w / 2, p.y + p.h / 2)
            gear.wait(0.5)
            check('click selects the panel', selected == 'layout:sample', selected)
            drag_ui(p.x + p.w - 6, p.y + p.h - 6, p.x + p.w - 6 + 60, p.y + p.h - 6 + 30)
            gear.wait(0.3)
            local q = sample()
            check('panel resized at its corner', q and math.abs(q.w - (p.w + 60)) < 3 and math.abs(q.h - (p.h + 30)) < 3,
                q and ('%dx%d -> %dx%d'):format(p.w, p.h, q.w, q.h))
            -- The popup's Width and Height sliders (a click at their middle) and its Reset button.
            gear.wait(0.3)
            local st = ui.state()
            local pr = popup_rect
            check('popup shown', pr ~= nil)
            if pr then
                A.ui_click(pr.x + 58 + 60, pr.y + 66 + 8)
                gear.wait(0.5)
                q = sample()
                local ew = p.min_width + 0.5 * (math.floor(st.screen_right - st.screen_left) - p.min_width)
                check('width slider', q and math.abs(q.w - ew) <= 4, q and ('%d, expected %.0f'):format(q.w, ew))
                pr = popup_rect
                A.ui_click(pr.x + 58 + 60, pr.y + 88 + 8)
                gear.wait(0.5)
                q = sample()
                local eh = p.min_height + 0.5 * (math.floor(st.screen_bottom - st.screen_top) - p.min_height)
                check('height slider', q and math.abs(q.h - eh) <= 4, q and ('%d, expected %.0f'):format(q.h, eh))
                A.screenshot('editmode-sliders')
                pr = popup_rect
                A.ui_click(pr.x + 8 + 60, pr.y + 112 + 11)
                gear.wait(0.5)
                q = sample()
                check('reset button', q and math.abs(q.x - 60) < 1 and math.abs(q.y - 120) < 1 and math.abs(q.w - 200) < 1
                    and math.abs(q.h - 110) < 1,
                    q and ('%d,%d %dx%d'):format(q.x, q.y, q.w, q.h))
                check('still selected after reset', selected == 'layout:sample', selected)
            end
            p = sample()
            drag_ui(p.x + 40, p.y + 40, p.x + 40 + 200, p.y + 40 + 150)
            gear.wait(0.3)
            q = sample()
            check('panel dragged', q and math.abs(q.x - (p.x + 200)) <= SNAP + 1 and math.abs(q.y - (p.y + 150)) <= SNAP + 1,
                q and ('%d,%d -> %d,%d'):format(p.x, p.y, q.x, q.y))
            A.screenshot('editmode-popup')
        end

        -- A click on empty space deselects.
        local ex, ey = empty_spot()
        check('found an empty spot', ex ~= nil)
        if ex then
            A.ui_click(ex, ey)
            gear.wait(0.5)
            check('click on empty space deselects', selected == nil, selected)
        end
        A.screenshot('editmode-deselected')

        -- The toolbar keeps its own title bar drag.
        local t = toolbar_rect
        if t then
            drag_ui(t.x + 40, t.y + 10, t.x + 40 + 250, t.y + 10 + 120)
            local t2 = toolbar_rect
            check('toolbar dragged', t2 and math.abs(t2.x - (t.x + 250)) < 3, t2 and ('%d -> %d'):format(t.x, t2.x))
        end
        A.press('f7')
        gear.wait(0.8)
        check('F7 ends the edit mode', not ui.edit_mode())
        A.screenshot('editmode-done')
        local l2 = ui.layout()
        check('stored', l2['game:UI_WND_INVENTORY'] ~= nil and l2['layout:sample'] ~= nil and l2['layout:toolbar'] ~= nil,
            gear.json.encode(l2))
    else
        local w, x, y = inventory()
        local e = lay['game:UI_WND_INVENTORY']
        local ex, ey = ui.from_virtual(e.x, e.y)
        check('inventory where it was left', x and math.abs(x - ex) < 3 and math.abs(y - ey) < 3,
            ('%s,%s expected %.0f,%.0f'):format(tostring(x), tostring(y), ex, ey))
        local p, s = sample(), lay['layout:sample']
        local sx, sy = ui.from_virtual(s.x, s.y)
        check('panel where it was left', p and math.abs(p.x - sx) < 3 and math.abs(p.y - sy) < 3)
        A.screenshot('editmode-restored')
        ui.set_edit_mode(true)
        gear.wait(0.5)
        A.screenshot('editmode-restored-on')
        ui.reset_layout()
        ui.set_edit_mode(false)
        gear.wait(0.8)
        local _, x3, y3 = inventory()
        check('reset puts it back', x3 and (math.abs(x3 - x) > 3 or math.abs(y3 - y) > 3),
            ('%s,%s -> %s,%s'):format(tostring(x), tostring(y), tostring(x3), tostring(y3)))
        A.screenshot('editmode-reset')
    end
    A.press('i')
    gear.wait(0.5)
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    A.quit(results.failed == 0 and 0 or 1)
end)
