-- Sacred Gear's mod manager: a screen listing the installed mods (switch on and off, their settings and key binds),
-- drawn with gear.ui widgets. It is a mod of its own that ships in SacredGear.dll (the kernel loads it); it gets the
-- extra global `admin` the kernel gives only to it. Opened with its hotkey (F11, rebindable in this screen) or the
-- "Mods" button beside the game's escape menu.

local ui = gear.ui
local C = ui.COLORS

local ROW_H, ROW_STEP = 66, 70   -- a mod's row in the list and the distance between rows
local SETTING_ROWS = 9      -- rows of settings per page (section headers count)
local KEY_ROWS = 12         -- rows (key binds and headings) per page
local W, H_MAX = 640, 482   -- the panel: its height is the most it takes, less on a small screen
local FOOTER_TOP = 42      -- the footer band: from this far above the panel's bottom edge down to it
local FOOTER_ITEM_H = 22   -- footer controls are this tall and centered in the band (see footer_y)
local LAYER = {layer = 'top'}

local RED, AMBER, GREEN, DIM = 0xFFFF7070, 0xFFFFC060, 0xFF90E890, C.text_dim

local screen = {
    open = false, view = 'list', page = 1, mod = nil, tab = 'settings', tab_page = 1,
    capture = nil, edit = nil, edit_text = '', restart = false,
}

---------------------------------------------------------------------------------------------------------------
-- Helpers
---------------------------------------------------------------------------------------------------------------
-- The footer band of a panel: [p.y + p.h - FOOTER_TOP, p.y + p.h). Its items share one top edge, footer_y, so they
-- sit on one line; the lists end above the band.
local function footer_y(p) return p.y + p.h - FOOTER_TOP + math.floor((FOOTER_TOP - 10 - FOOTER_ITEM_H) / 2) + 4 end
local function footer_top(p) return p.y + p.h - FOOTER_TOP end
-- How many rows of ROW_STEP fit between `top` and the footer band (a gap of 6 above the band).
local function rows_fitting(p, top, step, row_h)
    return math.max(1, math.floor((footer_top(p) - 6 - top - row_h) / step) + 1)
end
local measured = {}
local function text_w(text, opts)
    local key = text .. '\0' .. tostring(opts and opts.size) .. tostring(opts and opts.bold)
    local w = measured[key]
    if not w then
        w = ui.text_size(text, opts)
        if not w then return #text * 6 end      -- fonts not ready yet: a guess, not kept
        measured[key] = w
    end
    return w
end

-- `text` cut to `width` layout units, with "..." when it does not fit.
local function fit(text, width, opts)
    if text_w(text, opts) <= width then return text end
    local lo, hi = 0, #text
    while lo < hi do
        local mid = math.ceil((lo + hi) / 2)
        if text_w(text:sub(1, mid) .. '...', opts) <= width then lo = mid else hi = mid - 1 end
    end
    return text:sub(1, lo) .. '...'
end

local function format_binding(b)
    if b == nil or b == '' then return '(unbound)' end
    local out = {}
    for part in b:gmatch('[^%+]+') do out[#out + 1] = part:sub(1, 1):upper() .. part:sub(2) end
    return table.concat(out, '+')
end

local function find_entry(id)
    for _, e in ipairs(admin.entries()) do
        if e.id == id then return e end
    end
end

local function stop_input()
    screen.edit = nil
    screen.capture = nil
    screen.osk = nil
end

local function open_screen()
    screen.holding, screen.up_at = false, nil
    screen.open = true
    screen.view, screen.message = 'list', nil
    stop_input()
    ui.show_panel('main', true)
    ui.nav_modal(true)
    admin.modal(true)
end

-- `by_escape`: the keyboard stays kept from the game until the key is up (and a moment longer): the game's message
-- loop may still hold the Esc that closed the screen, and would open its escape menu with it.
local function close_screen(by_escape)
    screen.open = false
    stop_input()
    ui.show_panel('main', false)
    ui.nav_modal(false)
    if by_escape then
        screen.holding, screen.up_at = true, nil
    else
        admin.modal(false)
    end
end

local function toggle_screen()
    if screen.open then close_screen() else open_screen() end
end

local function paging(id, x, y, w, page, pages)
    -- [<]  Page x/y  [>] centered in w; returns the new page.
    local t = ui.table('pg:' .. id, x + math.floor(w / 2) - 90, y, 180,
        {columns = {30, '*', 30}, spacing = {5, 0}, row_height = FOOTER_ITEM_H, layer = 'top'})
    t:row()
    if t:button(1, '<', {id = id .. '<', enabled = page > 1, tooltip = 'Previous page'}) then
        page = page - 1
        stop_input()
    end
    t:label(2, ('Page %d / %d'):format(page, pages), {id = id .. '#n', halign = 'center', color = C.text})
    if t:button(3, '>', {id = id .. '>', enabled = page < pages, tooltip = 'Next page'}) then
        page = page + 1
        stop_input()
    end
    return math.max(1, math.min(pages, page))
end

---------------------------------------------------------------------------------------------------------------
-- The list
---------------------------------------------------------------------------------------------------------------
local function status_of(e)
    if e.state == 'invalid' then return 'Cannot be loaded: ' .. tostring(e.reason), RED end
    if e.state == 'error' then
        if e.reason then return 'Failed to start: ' .. e.reason, RED end
        return ('Switched off after %d errors (see SacredGear.log). Switch it off and on to try again.')
            :format(math.max(e.errors, 10)), RED
    end
    if e.state == 'blocked' then return 'Off: ' .. tostring(e.reason), AMBER end
    if e.state == 'pending' then return 'Starting...', DIM end
    return nil
end

local function key_conflicts_of(e)
    local n = 0
    for _, k in ipairs(admin.keys(e.id)) do
        n = n + #admin.conflicts(e.id, k.id)
    end
    return n
end

local function draw_row(e, x, y, w)
    local bad = e.state == 'error' or e.state == 'invalid'
    ui.rect(x, y, w, ROW_H, bad and 0x60401818 or 0x50181824, LAYER)
    ui.frame(x, y, w, ROW_H, bad and 0xFF804040 or C.edge_dim, 1, LAYER)
    local sid = e.id
    local dim = e.state == 'off' or e.state == 'blocked'
    -- switch | name, version and author | Settings | Keys; the description and the status under the name.
    local t = ui.table('row:' .. sid, x + 8, y + 8, w - 18, {
        columns = {44, {width = 'auto', max = 280}, '*', 80, 80},
        spacing = {6, 2}, row_heights = {22, 16, 14}, layer = 'top',
    })
    t:row()
    if e.builtin or e.state == 'invalid' then
        t:label(1, e.builtin and 'Core' or '', {halign = 'center', color = DIM})
    else
        local on = e.enabled
        if t:button(1, on and 'On' or 'Off', {id = 'en:' .. sid, height = 20, color = on and GREEN or RED,
            tooltip = on and 'Switch this mod off' or 'Switch this mod on'}) then
            local ok, why = admin.set_enabled(sid, not on)
            if not ok then screen.message = why end
        end
    end
    t:label(2, e.name, {size = 14, bold = true, color = dim and DIM or C.accent})
    local meta = ''
    if e.version ~= '' then meta = 'v' .. e.version end
    if e.author ~= '' then meta = meta .. (meta ~= '' and '  by ' or 'by ') .. e.author end
    t:label(3, meta, {size = 11, color = DIM})
    local can_settings = e.state ~= 'invalid' and (e.has_settings or e.has_page)
    if t:button(4, 'Settings', {id = 'set:' .. sid, enabled = can_settings,
        tooltip = can_settings and nil or 'This mod has no settings'}) then
        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, (e.has_settings and 'settings' or 'page'), 1
        stop_input()
    end
    if t:button(5, 'Keys', {id = 'keys:' .. sid, enabled = e.has_keys,
        tooltip = e.has_keys and nil or 'This mod declares no keys'}) then
        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, 'keys', 1
        stop_input()
    end
    t:row()
    t:label(2, e.description ~= '' and e.description or '(no description)', {span = 2, color = dim and DIM or C.text})
    local status, color = status_of(e)
    if not status then
        local conflicts = key_conflicts_of(e)
        if conflicts > 0 then
            status, color = 'Key conflict: another mod or the game uses the same key (see Keys)', AMBER
        elseif #e.requires > 0 then
            status, color = 'Requires ' .. table.concat(e.requires, ', '), DIM
        end
    end
    if status then
        t:row()
        t:label(2, status, {span = 4, size = 11, color = color})
    end
end

local function draw_list(p)
    local list = admin.entries()
    local x, y, w = p.content_x, p.content_y, p.content_w
    ui.text(('%d installed'):format(#list - 1), x, y, {height = 20, valign = 'middle', color = DIM, layer = 'top'})
    if screen.message then
        ui.text(fit(screen.message, w - 120), x + 90, y, {height = 20, valign = 'middle', color = AMBER, layer = 'top'})
    end
    y = y + 24
    local per_page = rows_fitting(p, y, ROW_STEP, ROW_H)
    local pages = math.max(1, math.ceil(#list / per_page))
    screen.page = math.max(1, math.min(pages, screen.page))
    local first = (screen.page - 1) * per_page
    for i = 1, per_page do
        local e = list[first + i]
        if not e then break end
        draw_row(e, x, y + (i - 1) * ROW_STEP, w)
    end
    -- footer: a note at the left, the pager in the middle, Close at the right
    local foot = ui.table('list_foot', x, footer_y(p), w, {columns = {'*', 180, '*'}, spacing = {0, 0},
        row_height = FOOTER_ITEM_H, layer = 'top'})
    foot:row()
    foot:label(1, 'Saved in mods\\gear_mods.json', {size = 11, color = DIM})
    local cx, cy, cw = foot:cell(2)
    screen.page = paging('lp', cx, cy, cw, screen.page, pages)
    if foot:button(3, 'Close', {id = 'close', width = 80, halign = 'right'}) then close_screen() end
end

---------------------------------------------------------------------------------------------------------------
-- Settings of one mod
---------------------------------------------------------------------------------------------------------------
local TYPED = {}
for c = string.byte('a'), string.byte('z') do TYPED[#TYPED + 1] = {key = string.char(c), plain = string.char(c), shift = string.char(c - 32)} end
for c = string.byte('0'), string.byte('9') do TYPED[#TYPED + 1] = {key = string.char(c), plain = string.char(c)} end
for _, k in ipairs({{'space', ' '}, {'minus', '-', '_'}, {'period', '.', ':'}, {'comma', ',', ';'}, {'slash', '/', '?'},
                    {'equals', '=', '+'}, {'semicolon', ';', ':'}, {'quote', "'", '"'}, {'backslash', '\\', '|'}}) do
    TYPED[#TYPED + 1] = {key = k[1], plain = k[2], shift = k[3]}
end
local backspace_since

-- Typing into the focused text field: every key goes straight into the setting.
local function process_typing(max_length)
    local text = screen.edit_text
    local changed = false
    local shift = ui.key_down('shift')
    for _, k in ipairs(TYPED) do
        if ui.key_pressed(k.key) and #text < max_length then
            text = text .. ((shift and k.shift) or k.plain)
            changed = true
        end
    end
    if ui.key_down('backspace') then
        local now = gear.time()
        if not backspace_since then
            backspace_since = now
            text = text:sub(1, -2)
            changed = true
        elseif now - backspace_since > 0.45 then
            backspace_since = now - 0.4
            text = text:sub(1, -2)
            changed = true
        end
    else
        backspace_since = nil
    end
    screen.edit_text = text
    return changed
end

-- The game's own names in a label: `{ability:N}` becomes the name of combat art / spell N in the game's language.
local function game_names(text)
    if type(text) ~= 'string' or not text:find('{ability:', 1, true) then return text end
    return (text:gsub('{ability:(%d+)}', function(n)
        local rec = gear.data and gear.data.abilities and gear.data.abilities[tonumber(n)]
        local name = rec and rec.name_text and gear.text and gear.text.get(rec.name_text)
        return name or ('#' .. n)
    end))
end

local function setting_row(t, m, d)
    local id = 's:' .. m .. ':' .. d.id
    local value = admin.get(m, d.id)
    local label = game_names(d.label) .. (d.restart and '  (after restart)' or '')
    -- a label longer than its column is cut (the whole of it in the tooltip), so it never runs into the control
    local tip = game_names(d.tooltip)
    local _, _, lw = t:cell(1)
    if text_w(label) > lw then tip = label .. (tip and ('\n' .. tip) or '') end
    t:label(1, label, {id = id .. '#l', tooltip = tip})
    local changed, nv = false, value
    if d.type == 'bool' then
        changed, nv = t:checkbox(2, value and 'On' or 'Off', value, {id = id})
    elseif d.type == 'number' then
        changed, nv = t:slider(2, d.min, d.max, value, {id = id, width = 200, step = d.step})
    elseif d.type == 'choice' then
        local labels, index = {}, 1
        for i, c in ipairs(d.choices) do
            labels[i] = c.label
            if c.value == value then index = i end
        end
        local ch, i = t:dropdown(2, labels, index, {id = id, width = 220, height = 18})
        if ch then changed, nv = true, d.choices[i].value end
    elseif d.type == 'text' then
        -- the field (200 x 20) and the on-screen keyboard button, in a table of their own inside the cell
        local cx, cy, cw = t:cell(2)
        local f = ui.table(id .. '#f', cx, cy, cw, {columns = {200, 40}, spacing = {4, 0}, row_height = 24, layer = 'top'})
        f:row()
        local fx, fy = f:cell(1)
        fy = fy + 2
        local focused = screen.edit == id
        local r = ui.region(id, fx, fy, 200, 20, LAYER)
        ui.rect(fx, fy, 200, 20, focused and 0xF0202030 or C.bg, LAYER)
        ui.frame(fx, fy, 200, 20, (focused or r.hover) and C.edge or C.edge_dim, 1, LAYER)
        if f:button(2, 'Kbd', {id = id .. '#kbd', height = 20,
            tooltip = 'Type with an on-screen keyboard (for a controller)'}) then
            screen.osk = {mode = 'text', mod = m, def = d, label = d.label, text = value, max = d.max_length or 64}
            screen.edit, screen.capture = nil, nil
        end
        if r.clicked and not focused then
            screen.edit, screen.edit_text, screen.capture = id, value, nil
            focused = true
        end
        local shown = focused and screen.edit_text or value
        local blink = focused and math.floor(gear.time() * 2) % 2 == 0 and '|' or ''
        ui.text(fit(shown, 188) .. blink, fx + 5, fy, {height = 20, valign = 'middle', layer = 'top'})
        if focused then
            if ui.key_pressed('enter') or ui.key_pressed('tab') then
                screen.edit = nil
            elseif process_typing(d.max_length) then
                changed, nv = true, screen.edit_text
            end
        end
    elseif d.type == 'color' then
        -- the swatch and four channel sliders, again a table inside the cell
        local a, r, g, b = bit.rshift(value, 24) % 256, bit.rshift(value, 16) % 256, bit.rshift(value, 8) % 256, value % 256
        local cx, cy, cw = t:cell(2)
        local f = ui.table(id .. '#c', cx, cy, cw, {columns = {22, 54, 54, 54, 54}, spacing = {8, 0}, row_height = 24,
            layer = 'top'})
        f:row()
        local sx, sy = f:cell(1)
        ui.rect(sx, sy + 3, 22, 18, ui.rgba(255, 255, 255, 255), LAYER)
        ui.rect(sx, sy + 3, 22, 18, value, LAYER)
        ui.frame(sx, sy + 3, 22, 18, C.edge, 1, LAYER)
        local chans = {{'a', 'Alpha', a}, {'r', 'Red', r}, {'g', 'Green', g}, {'b', 'Blue', b}}
        for i, c in ipairs(chans) do
            local ch, v = f:slider(1 + i, 0, 255, c[3], {id = id .. c[1], step = 1, format = false,
                tooltip = c[2] .. ' ' .. c[3]})
            if ch then
                chans[i][3] = v
                changed = true
            end
        end
        if changed then nv = ui.rgba(chans[2][3], chans[3][3], chans[4][3], chans[1][3]) end
    end
    if changed then
        admin.set(m, d.id, nv)
        if d.restart then screen.restart = true end
    end
    if value ~= admin.default(m, d.id) then
        if t:button(3, 'Reset', {id = id .. '#reset', height = 20, tooltip = 'Back to the default'}) then
            admin.set(m, d.id, admin.default(m, d.id))
        end
    end
end

-- The rows of a settings page: section headers and settings, in pages that never end on a header.
local function setting_pages(defs)
    local pages, cur, last_section = {{}}, 1, nil
    local function add(item)
        local page = pages[#pages]
        if #page >= SETTING_ROWS then page = {}; pages[#pages + 1] = page end
        page[#page + 1] = item
    end
    for _, d in ipairs(defs) do
        if d.section ~= last_section then
            last_section = d.section
            if d.section then
                local page = pages[#pages]
                if #page >= SETTING_ROWS - 1 then pages[#pages + 1] = {} end
                add({header = d.section})
            end
        end
        -- a setting's note: a line of explanation above it (for a section, on its first setting)
        if d.note then add({note_row = game_names(d.note)}) end
        add(d)
    end
    return pages
end

local function draw_settings(p, e)
    local x, y, w = p.content_x, p.content_y + 34, p.content_w
    local defs = admin.settings(e.id)
    local pages = setting_pages(defs)
    screen.tab_page = math.max(1, math.min(#pages, screen.tab_page))
    local items = pages[screen.tab_page]
    -- label | control | Reset: the controls start 290 in. (An open dropdown's list is drawn on top by itself.)
    local t = ui.table('settings', x + 6, y, w - 10, {columns = {278, '*', 50}, spacing = {6, 8}, row_height = 24,
        layer = 'top'})
    for _, item in ipairs(items) do
        t:row()
        if item.header then
            local hx, hy, hw = t:cell(1, 3)
            t:label(1, item.header, {id = 'h:' .. item.header, span = 3, size = 13, bold = true, color = C.accent})
            local tw = text_w(item.header, {size = 13, bold = true})
            ui.rect(hx + tw + 8, hy + 11, hw - tw - 8, 1, C.edge_dim, LAYER)
        elseif item.note_row then
            t:label(1, item.note_row, {id = 'note:' .. item.note_row, span = 3, size = 12, color = C.dim or 0xFFA0A0A0})
        else
            setting_row(t, e.id, item)
        end
    end
    local foot = ui.table('settings_foot', x, footer_y(p), w, {columns = {'*', 180, '*'}, spacing = {0, 0},
        row_height = FOOTER_ITEM_H, layer = 'top'})
    foot:row()
    if foot:button(1, 'Reset to defaults', {id = 'reset_all', width = 130,
        tooltip = 'Every setting of this mod back to its default'}) then
        admin.reset(e.id)
    end
    if #pages > 1 then
        local cx, cy, cw = foot:cell(2)
        screen.tab_page = paging('sp', cx, cy, cw, screen.tab_page, #pages)
    end
    if screen.restart then
        foot:label(3, 'Some changes apply after a restart', {id = 'restart', halign = 'right', size = 11, color = AMBER})
    end
end

---------------------------------------------------------------------------------------------------------------
-- Key binds of one mod
---------------------------------------------------------------------------------------------------------------
-- One slot of a key row: the binding's button (click, then press the new key) and the pick button for a controller,
-- in columns `col` and `col + 1` of the row's table.
local function key_slot(t, col, e, k, id, conflicts)
    local binding = admin.binding(e.id, k.id)
    local capturing = screen.capture and screen.capture.mod == e.id and screen.capture.key == k.id
    local text = capturing and 'Press a key...' or format_binding(binding)
    local tip = 'Click, then press the new key. Esc cancels, Backspace unbinds.'
    if #conflicts > 0 then
        local names = {}
        for _, c in ipairs(conflicts) do names[#names + 1] = c.name .. ' / ' .. c.label end
        tip = 'Conflicts with: ' .. table.concat(names, ', ')
    end
    if t:button(col, text, {id = id, height = 20,
        color = capturing and C.accent or (#conflicts > 0 and AMBER or nil), tooltip = tip}) then
        screen.capture = {mod = e.id, key = k.id}
        screen.edit = nil
        admin.capture_start()
    end
    if t:button(col + 1, '...', {id = id .. '#pick', height = 20,
        tooltip = 'Choose the key from an on-screen list (for a controller)'}) then
        screen.osk = {mode = 'key', mod = e.id, key = k.id, label = k.label, ctrl = false, shift = false, alt = false}
        screen.capture, screen.edit = nil, nil
    end
end

local function draw_keys(p, e)
    local x, y, w = p.content_x, p.content_y + 30, p.content_w
    -- Rows: a heading where a key's section changes, then the key (a key that is another's second slot shares its row).
    local rows, section = {}, nil
    for _, k in ipairs(admin.keys(e.id)) do
        if not k.is_second then
            if k.section and k.section ~= section then rows[#rows + 1] = {head = k.section} end
            if k.section then section = k.section end
            rows[#rows + 1] = {key = k}
        end
    end
    local pages = math.max(1, math.ceil(#rows / KEY_ROWS))
    screen.tab_page = math.max(1, math.min(pages, screen.tab_page))
    -- capturing a new binding
    local cap = screen.capture
    if cap and cap.mod == e.id then
        local b = admin.capture()
        if b then
            if b == 'backspace' or b == 'delete' then b = '' end
            admin.set_binding(cap.mod, cap.key, b)
            screen.capture = nil
        end
    end
    local first = (screen.tab_page - 1) * KEY_ROWS
    for i = 1, KEY_ROWS do
        local row = rows[first + i]
        if not row then break end
        local ry = y + (i - 1) * 24
        local second = row.key and row.key.second and admin.key_def(e.id, row.key.second) or nil
        -- label | slot, pick | (slot, pick) | Default
        local cols = {'*'}
        if row.key then cols = second and {214, 96, 22, 96, 22, 58} or {214, 150, 22, 58} end
        local t = ui.table('kr:' .. i, x + 6, ry, w - 10, {columns = cols, spacing = {4, 0}, row_height = 22, layer = 'top'})
        t:row()
        if row.head then
            t:label(1, row.head, {id = 'kh:' .. row.head, color = C.accent})
        else
            local k = row.key
            local id = 'k:' .. e.id .. ':' .. k.id
            local conflicts = admin.conflicts(e.id, k.id)
            local conflicts2 = second and admin.conflicts(e.id, second.id) or {}
            local names = {}
            for _, list in ipairs({conflicts, conflicts2}) do
                for _, c in ipairs(list) do names[#names + 1] = c.name .. ' / ' .. c.label end
            end
            t:label(1, k.label, {id = id .. '#l', color = #names > 0 and AMBER or nil,
                tooltip = (#names > 0 and ('Conflicts with: ' .. table.concat(names, ', ') .. '. ') or '') ..
                    'Default: ' .. format_binding(admin.default_binding(e.id, k.id))})
            key_slot(t, 2, e, k, id, conflicts)
            if second then key_slot(t, 4, e, second, 'k:' .. e.id .. ':' .. second.id, conflicts2) end
            local changed = admin.binding(e.id, k.id) ~= admin.default_binding(e.id, k.id) or
                (second and admin.binding(e.id, second.id) ~= admin.default_binding(e.id, second.id))
            if changed and t:button(#cols, 'Default', {id = id .. '#reset', height = 20,
                tooltip = 'Back to the default key' .. (second and 's' or '')}) then
                admin.set_binding(e.id, k.id, nil)
                if second then admin.set_binding(e.id, second.id, nil) end
            end
        end
    end
    local fy = footer_y(p)
    ui.text('Click a key, then press the new one. Esc cancels, Backspace unbinds.', x, fy,
        {height = FOOTER_ITEM_H, valign = 'middle', size = 11, color = DIM, layer = 'top'})
    if pages > 1 then screen.tab_page = paging('kp', x + w - 280, fy, 200, screen.tab_page, pages) end
end

---------------------------------------------------------------------------------------------------------------
-- A mod's screen: tabs
---------------------------------------------------------------------------------------------------------------
local function draw_mod(p)
    local e = find_entry(screen.mod)
    if not e then screen.view = 'list'; return end
    local x, y, w = p.content_x, p.content_y, p.content_w
    local tabs = {}
    if e.has_settings then tabs[#tabs + 1] = {'settings', 'Settings'} end
    if e.has_keys then tabs[#tabs + 1] = {'keys', 'Keys'} end
    if e.has_page then tabs[#tabs + 1] = {'page', 'More'} end
    local present = false
    for _, t in ipairs(tabs) do if t[1] == screen.tab then present = true end end
    if not present and tabs[1] then screen.tab = tabs[1][1] end
    -- [< Back]  the mod's name ........................ [tab] [tab] [tab]
    local cols = {70, '*'}
    for _ = 1, #tabs do cols[#cols + 1] = 80 end
    local head = ui.table('mod_head', x, y, w - 4, {columns = cols, spacing = {4, 0}, row_height = 22, layer = 'top'})
    head:row()
    if head:button(1, '< Back', {id = 'back'}) then
        screen.view = 'list'
        stop_input()
        return
    end
    head:label(2, e.name, {size = 14, bold = true, color = C.accent})
    for i, t in ipairs(tabs) do
        local active = screen.tab == t[1]
        if head:button(2 + i, t[2], {id = 'tab:' .. t[1], color = active and C.accent or nil}) and not active then
            screen.tab, screen.tab_page = t[1], 1
            stop_input()
        end
        if active then
            local tx, ty, tw, th = head:cell(2 + i)
            ui.rect(tx, ty + th, tw, 2, C.accent, LAYER)
        end
    end
    if #tabs == 0 then
        ui.text('Nothing to set here.', x + 6, y + 40, {color = DIM, layer = 'top'})
    elseif screen.tab == 'settings' then
        draw_settings(p, e)
    elseif screen.tab == 'keys' then
        draw_keys(p, e)
    elseif screen.tab == 'page' then
        admin.draw_page(e.id, x, y + 34, w, p.h - 34 - 34 - 28)
    end
end

---------------------------------------------------------------------------------------------------------------
-- An on-screen keyboard (for the controller): types into a text setting, or picks a key for a bind.
---------------------------------------------------------------------------------------------------------------
local OSK_TEXT = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-'},
    {'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', '_', ':'},
}
local OSK_F = {'f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f10', 'f11', 'f12'}
local OSK_SPECIAL = {'space', 'tab', 'enter', 'insert', 'delete', 'home', 'end', 'pageup', 'pagedown', 'up', 'down',
                     'left', 'right'}
local OSK_SHORT = {space = 'Spc', pageup = 'PgUp', pagedown = 'PgDn', insert = 'Ins', delete = 'Del'}

-- A row of keys `kw` wide, centered in the panel's content; returns the key clicked.
local function osk_row(p, id, y, list, kw, label)
    local gap = 3
    local cols = {}
    for i = 1, #list do cols[i] = kw end
    local total = #list * kw + (#list - 1) * gap
    local t = ui.table('osk:' .. id, p.content_x + math.floor((p.content_w - total) / 2), y, total,
        {columns = cols, spacing = {gap, 0}, row_height = 26, layer = 'top'})
    t:row()
    local hit
    for i, k in ipairs(list) do
        if t:button(i, label and label(k) or k, {id = 'osk:' .. id .. k}) then hit = k end
    end
    return hit
end

local function draw_osk(p)
    local o = screen.osk
    local x, y, w = p.content_x, p.content_y, p.content_w
    local what = o.mode == 'text' and ('Typing: ' .. o.label) or ('Pick a key for: ' .. o.label)
    ui.text(fit(what, w), x, y, {size = 14, bold = true, color = C.accent, height = 22, valign = 'middle', layer = 'top'})
    y = y + 28
    if o.mode == 'text' then
        ui.rect(x, y, w, 24, C.bg, LAYER)
        ui.frame(x, y, w, 24, C.edge, 1, LAYER)
        ui.text(fit(o.text, w - 12) .. '|', x + 6, y, {height = 24, valign = 'middle', layer = 'top'})
        y = y + 32
        for i, row in ipairs(OSK_TEXT) do
            local hit = osk_row(p, 'r' .. i, y, row, 40, function(k)
                return (o.shift and k:match('%a')) and k:upper() or k
            end)
            if hit and #o.text < o.max then
                o.text = o.text .. ((o.shift and hit:match('%a')) and hit:upper() or hit)
                o.shift = false
                admin.set(o.mod, o.def.id, o.text)
            end
            y = y + 30
        end
        local t = ui.table('osk:bottom', x + 40, y, nil, {columns = {90, 180, 110, 110}, spacing = {8, 0}, row_height = 26,
            layer = 'top'})
        t:row()
        if t:button(1, o.shift and 'SHIFT' or 'Shift', {id = 'osk:shift', color = o.shift and C.accent or nil}) then
            o.shift = not o.shift
        end
        if t:button(2, 'Space', {id = 'osk:space'}) and #o.text < o.max then
            o.text = o.text .. ' '
            admin.set(o.mod, o.def.id, o.text)
        end
        if t:button(3, 'Backspace', {id = 'osk:back'}) then
            o.text = o.text:sub(1, -2)
            admin.set(o.mod, o.def.id, o.text)
        end
        if t:button(4, 'Done', {id = 'osk:done'}) then screen.osk = nil end
    else
        local names = {ctrl = 'Ctrl', shift = 'Shift', alt = 'Alt'}
        -- the modifiers, a gap, Unbind and Cancel
        local t = ui.table('osk:mods', x + 40, y, nil, {columns = {80, 80, 80, 12, 110, 110}, spacing = {8, 0},
            row_height = 26, layer = 'top'})
        t:row()
        for i, m in ipairs({'ctrl', 'shift', 'alt'}) do
            if t:button(i, names[m], {id = 'osk:mod' .. m, color = o[m] and C.accent or nil}) then o[m] = not o[m] end
        end
        if t:button(5, 'Unbind', {id = 'osk:none'}) then
            admin.set_binding(o.mod, o.key, '')
            screen.osk = nil
            return
        end
        if t:button(6, 'Cancel', {id = 'osk:cancel'}) then
            screen.osk = nil
            return
        end
        y = y + 34
        local hit
        for i, row in ipairs(OSK_TEXT) do
            hit = osk_row(p, 'r' .. i, y, row, 40) or hit
            y = y + 30
        end
        hit = osk_row(p, 'f', y, OSK_F, 42, function(k) return k:upper() end) or hit
        y = y + 30
        hit = osk_row(p, 's', y, OSK_SPECIAL, 42, function(k)
            return OSK_SHORT[k] or (k:sub(1, 1):upper() .. k:sub(2, 3))
        end) or hit
        if hit then
            local b = (o.ctrl and 'ctrl+' or '') .. (o.shift and 'shift+' or '') .. (o.alt and 'alt+' or '') .. hit
            admin.set_binding(o.mod, o.key, b)
            screen.osk = nil
            return
        end
        ui.text('Pick the modifiers first, then the key.', x, y + 34, {size = 11, color = DIM, layer = 'top'})
    end
end

---------------------------------------------------------------------------------------------------------------
-- The mod menu (the controller's button): every mod's hotkey actions and tools as buttons
---------------------------------------------------------------------------------------------------------------
local menu = {open = false, page = 1}
local MENU_ROWS = 8

local function draw_menu()
    local entries = {{label = 'Mod manager', run = function() menu.open = false; open_screen() end}}
    for _, e in ipairs(admin.menu_entries()) do entries[#entries + 1] = e end
    local pages = math.max(1, math.ceil(#entries / MENU_ROWS))
    menu.page = math.max(1, math.min(pages, menu.page))
    local l, t, r, b = ui.screen()
    local shown = math.min(MENU_ROWS, #entries)
    local mw, mh = 400, 36 + shown * 30 + (pages > 1 and 34 or 0)
    local p = ui.panel('padmenu', 'Mods', (l + r - mw) / 2, (t + b - mh) / 2, mw, mh,
        {layer = 'top', closable = true, back = false, draggable = false})
    if not p.open then
        menu.open = false
        return
    end
    local first = (menu.page - 1) * MENU_ROWS
    local list = ui.table('menu', p.content_x, p.content_y, p.content_w, {columns = {'*'}, spacing = {0, 4},
        row_height = 26, layer = 'top'})
    for i = 1, MENU_ROWS do
        local en = entries[first + i]
        if not en then break end
        list:row()
        if list:button(1, fit(en.label, p.content_w - 12), {id = 'menu:' .. (first + i),
            tooltip = en.mod and ('From ' .. en.mod) or nil}) then
            menu.open = false
            en.run()
            return
        end
    end
    if pages > 1 then
        local pager = ui.table('menu_pager', p.x + math.floor(p.w / 2) - 68, p.y + p.h - 34, 136,
            {columns = {30, 72, 30}, spacing = {4, 0}, row_height = 24, layer = 'top'})
        pager:row()
        if pager:button(1, '<', {id = 'menu:<', enabled = menu.page > 1}) then menu.page = menu.page - 1 end
        pager:label(2, menu.page .. ' / ' .. pages, {id = 'menu:n', halign = 'center'})
        if pager:button(3, '>', {id = 'menu:>', enabled = menu.page < pages}) then menu.page = menu.page + 1 end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Each frame
---------------------------------------------------------------------------------------------------------------
local function escape_menu_button(state)
    if state.mode == 'menus' then
        if ui.button('menu_button', 16, 724, 90, 26, 'Mods', {layer = 'top', tooltip = 'Mod manager'}) then open_screen() end
    elseif state.escape_menu and not state.message_box then
        local l, t, r, b = ui.screen()
        local x = (l + r) / 2 - 60      -- below the escape menu's window (centered in the layout)
        if ui.button('esc_button', x, 508, 120, 26, 'Mods', {layer = 'top', tooltip = 'Mod manager'}) then open_screen() end
    end
end

gear.on('draw', function(state)
    if screen.holding then
        if ui.key_down('escape') then
            screen.up_at = nil
        else
            screen.up_at = screen.up_at or gear.time()
            if gear.time() - screen.up_at > 0.3 then
                screen.holding, screen.up_at = false, nil
                admin.modal(false)
            end
        end
    end
    if admin.menu_pressed() and not screen.open then
        menu.open, menu.page = not menu.open, 1
        ui.show_panel('padmenu', true)
    end
    if menu.open and not screen.open then
        ui.nav_modal(true)
        if ui.back_pressed() then menu.open = false end
        if menu.open then
            draw_menu()
            return
        end
    end
    if not screen.open then
        ui.nav_modal(false)
        escape_menu_button(state)
        return
    end
    local back = ui.back_pressed()
    local escape = ui.key_pressed('escape')
    local l, t, r, b = ui.screen()
    local blocker = ui.region('blocker', l, t, r - l, b - t, {layer = 'top', nav = false})
    if blocker.pressed then stop_input() end
    local H = math.max(300, math.min(H_MAX, b - t - 16))
    local px, py = (l + r - W) / 2, math.max(t + 8, (t + b - H) / 2)
    local p = ui.panel('main', 'Mods', px, py, W, H, {layer = 'top', closable = true, back = false})
    if not p.open then
        close_screen()
        return
    end
    if back then
        -- The controller's B: out of the keyboard, out of a mod's screen, then out of the manager.
        if screen.osk then screen.osk = nil
        elseif screen.capture or screen.edit then stop_input()
        elseif screen.view ~= 'list' then screen.view = 'list'
        else close_screen() return end
    elseif escape then
        if screen.osk or screen.capture or screen.edit then stop_input() else close_screen(true) return end
    end
    if screen.osk then
        draw_osk(p)
    elseif screen.view == 'list' then
        draw_list(p)
    else
        draw_mod(p)
    end
end)

gear.keys.on('open', toggle_screen)

gear.on('unload', function() ui.nav_modal(false); admin.modal(false) end)
