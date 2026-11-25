-- Sacred Gear's mod manager: a screen listing the installed mods (switch on and off, their settings and key binds),
-- drawn with gear.ui widgets. It is a mod of its own that ships in SacredGear.dll (the kernel loads it); it gets the
-- extra global `admin` the kernel gives only to it. Opened with its hotkey (F11, rebindable in this screen) or the
-- "Mods" button beside the game's escape menu.

local paste_shown = false      -- whether the Paste button was drawn in the last frame (mods.query, for the self test)
local ui = gear.ui
local C = ui.COLORS

local ROW_H, ROW_H_STATUS, ROW_GAP = 52, 66, 4   -- a mod's entry in the list (taller with a status line) and the gap between entries
local SETTING_ROWS = 9      -- rows of settings per page (section headers count)
local KEY_ROWS = 12         -- rows (key binds and headings) per page
local W, H_MAX = 640, 482   -- the panel: its height is the most it takes, less on a small screen
local FOOTER_TOP = 42      -- the footer band: from this far above the panel's bottom edge down to it
local FOOTER_ITEM_H = 22   -- footer controls are this tall and centered in the band (see footer_y)
local LAYER = {layer = 'top'}

local RED, AMBER, GREEN, DIM = 0xFFFF7070, 0xFFFFC060, 0xFF90E890, C.text_dim

local NAME_OPTS = {size = 14, bold = true}
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

-- A text field in the game's pill shape: a rounded box (the fill and a 1 unit edge) built from one-unit rows, the ends of the rows pulled in on a
-- half circle (h is the diameter). `edge` and `fill` are colours.
local function pill(x, y, w, h, fill, edge)
    local rows = math.floor(h / 2) * 2          -- an even height: the two halves are mirror images
    local half, r = rows / 2, rows / 2
    local inset = {}
    for i = 0, half - 1 do
        local d = r - i - 0.5
        inset[i] = math.floor(r - math.sqrt(math.max(0, r * r - d * d)) + 0.5)
    end
    for i = 0, half - 1 do
        local step = (i == half - 1) and 1 or math.max(1, inset[i] - inset[i + 1])
        for _, row in ipairs({i, rows - 1 - i}) do
            local ins = inset[i]
            if i == 0 then
                ui.rect(x + ins, y + row, w - 2 * ins, 1, edge, LAYER)       -- the top and bottom edge
            else
                ui.rect(x + ins + step, y + row, w - 2 * (ins + step), 1, fill, LAYER)
                ui.rect(x + ins, y + row, step, 1, edge, LAYER)
                ui.rect(x + w - ins - step, y + row, step, 1, edge, LAYER)
            end
        end
    end
end

-- The blinking cursor of a text field: a bar at the foot of the text, as wide as a "_" (like the game's own fields), where the text ends.
local function underscore(text, x, y, h, color)
    if math.floor(gear.time() * 2) % 2 ~= 0 then return end
    ui.rect(x + 5 + text_w(text), y + h - 5, math.max(6, text_w('_')), 2, color, LAYER)
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
    screen.dd = nil
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

-- The line under an entry's description: why it is not running, a key conflict, or what it requires; nil for none. The
-- key conflicts cost a scan of every mod's keys, so a mod's line is kept for half a second.
local status_cache = {}
local function entry_status(e)
    local now = gear.time()
    local c = status_cache[e.id]
    if c and now - c.at < 0.5 and c.state == e.state then return c.status, c.color end
    local status, color = status_of(e)
    if not status then
        if key_conflicts_of(e) > 0 then
            status, color = 'Key conflict: another mod or the game uses the same key (see Keys)', AMBER
        elseif #e.requires > 0 then
            status, color = 'Requires ' .. table.concat(e.requires, ', '), DIM
        end
    end
    status_cache[e.id] = {at = now, state = e.state, status = status, color = color}
    return status, color
end

-- The entries of the list in pages: a page takes entries (each as tall as it needs) until the next would not fit in
-- `avail`, so a taller entry moves to the next page instead of reaching into the footer.
local function list_pages(list, avail)
    local pages, cur, used = {}, {}, 0
    for _, e in ipairs(list) do
        local status, color = entry_status(e)
        local h = status and ROW_H_STATUS or ROW_H
        if #cur > 0 and used + h > avail then
            pages[#pages + 1], cur, used = cur, {}, 0
        end
        cur[#cur + 1] = {e = e, h = h, status = status, color = color}
        used = used + h + ROW_GAP
    end
    pages[#pages + 1] = cur
    return pages
end

local function draw_row(e, x, y, w, h, status, color)
    local bad = e.state == 'error' or e.state == 'invalid'
    ui.rect(x, y, w, h, bad and 0x60401818 or 0x50181824, LAYER)
    ui.frame(x, y, w, h, bad and 0xFF804040 or C.edge_dim, 1, LAYER)
    local sid = e.id
    local dim = e.state == 'off' or e.state == 'blocked'
    -- switch | name, version and author | Settings | Keys; the description and the status under the name.
    local t = ui.table('row:' .. sid, x + 8, y + 8, w - 18, {
        columns = {44, math.min(math.ceil(text_w(e.name, NAME_OPTS)) + 1, 280), '*', 80, 80},
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
    local pages = list_pages(list, footer_top(p) - 6 - y)
    screen.page = math.max(1, math.min(#pages, screen.page))
    for _, it in ipairs(pages[screen.page]) do
        draw_row(it.e, x, y, w, it.h, it.status, it.color)
        y = y + it.h + ROW_GAP
    end
    -- footer: a note at the left, the pager in the middle, Close at the right
    local foot = ui.table('list_foot', x, footer_y(p), w, {columns = {'*', 180, '*'}, spacing = {0, 0},
        row_height = FOOTER_ITEM_H, layer = 'top'})
    foot:row()
    foot:label(1, 'Saved in mods\\gear_mods.json', {size = 11, color = DIM})
    local cx, cy, cw = foot:cell(2)
    screen.page = paging('lp', cx, cy, cw, screen.page, #pages)
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
        pill(fx, fy, 200, 20, focused and 0xF0202030 or C.bg, (focused or r.hover) and C.edge or C.edge_dim)
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
        local shown_fit = fit(shown, 176)
        ui.text(shown_fit, fx + 10, fy, {height = 20, valign = 'middle', layer = 'top'})
        if focused then underscore(shown_fit, fx + 5, fy, 20, C.accent) end
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
        pill(x, y, w, 24, C.bg, C.edge)
        local shown_fit = fit(o.text, w - 28)
        ui.text(shown_fit, x + 12, y, {height = 24, valign = 'middle', layer = 'top'})
        underscore(shown_fit, x + 7, y, 24, C.accent)
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
-- The screen as an RmlUi document (src/gear/lua/modmanager.rml: the game theme). The gear.ui drawing above stays as the fallback for when the
-- document cannot load (no RmlUi, or no theme mod running: it comes with the sacred-theme mod).
---------------------------------------------------------------------------------------------------------------
local draw_rml_screen
do
    local R = {doc = nil, retry = 0, acts = {}, slacts = {}, txacts = {}, pushed = {}, last = {}, built_at = -1, dirty = true}

    local function ser(v)
        local t = type(v)
        if t == 'table' then
            local keys = {}
            for k in pairs(v) do keys[#keys + 1] = k end
            table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
            local out = {}
            for _, k in ipairs(keys) do out[#out + 1] = tostring(k) .. '=' .. ser(v[k]) end
            return '{' .. table.concat(out, ',') .. '}'
        elseif t == 'string' then
            return string.format('%q', v)
        end
        return tostring(v)
    end

    -- a row of the model with every field the document reads (a field that is missing in some rows makes the bindings complain)
    local function row(kind)
        return {kind = kind, id = '', bool_id = '', num_id = '', text_id = '', label = '', tip = '', changed = false, rst_id = '', on = false, min = 0, max = 0, step = 1, value = 0,
            text = '', dd_id = '', open = false, opts = {{id = '', label = '', on = false}}, max_length = 64, kbd_id = '', css = 'transparent', a = 0, r = 0, g = 0, b = 0,
            a_id = '', r_id = '', g_id = '', b_id = '', slot = '', slot_id = '', pick_id = '', has2 = false, slot2 = '',
            slot2_id = '', pick2_id = '', capturing = false, capturing2 = false, conflict = false, conflict2 = false}
    end

    local function decimals(step)
        local s = ('%.4f'):format(step):gsub('0+$', '')
        local frac = s:match('%.(%d*)$')
        return frac and #frac or 0
    end

    -- the value on the slider's grid, inside its range (what the slider itself would report back)
    local function on_grid(d, v)
        v = math.max(d.min, math.min(d.max, v))
        if d.step and d.step > 0 then v = d.min + math.floor((v - d.min) / d.step + 0.5) * d.step end
        return math.max(d.min, math.min(d.max, v))
    end

    local function setting_rows(e, m)
        local defs = admin.settings(e.id)
        local acts, slacts, txacts = R.acts, R.slacts, R.txacts
        local last_section, n = nil, 0
        local function add(r) m.rows[#m.rows + 1] = r end
        for _, d in ipairs(defs) do
            if d.section ~= last_section then
                last_section = d.section
                if d.section then local r = row('head'); r.label = d.section; add(r) end
            end
            if d.note then local r = row('note'); r.label = game_names(d.note); add(r) end
            n = n + 1
            local id = 's' .. n
            local value = admin.get(e.id, d.id)
            local default = admin.default(e.id, d.id)
            local r = row(d.type == 'number' and 'num' or d.type)
            r.id = id
            r[(d.type == 'bool' and 'bool' or d.type == 'number' and 'num' or 'text') .. '_id'] = id
            R.index.settings[d.id] = r
            r.label = game_names(d.label) .. (d.restart and '  (after restart)' or '')
            r.tip = game_names(d.tooltip) or ''
            r.changed = value ~= default
            r.rst_id = id .. 'r'
            local function set(nv)
                admin.set(e.id, d.id, nv)
                if d.restart then screen.restart = true end
                R.dirty = true
            end
            acts[r.rst_id] = function() admin.set(e.id, d.id, default); R.dirty = true end
            if d.type == 'bool' then
                r.kind, r.on = 'bool', value and true or false
                acts[id] = function() set(not admin.get(e.id, d.id)) end
            elseif d.type == 'number' then
                r.kind = 'num'
                r.min, r.max, r.step = d.min, d.max, d.step or 1
                r.value = on_grid(d, value)
                r.text = ('%.' .. decimals(r.step) .. 'f'):format(value)
                R.pushed[id] = r.value
                slacts[id] = function(v)
                    if math.abs(v - (R.pushed[id] or 0)) < 1e-6 then return end
                    set(v)
                end
            elseif d.type == 'choice' then
                r.kind = 'choice'
                local index = 1
                for i, c in ipairs(d.choices) do if c.value == value then index = i end end
                r.text = d.choices[index].label
                -- a drop-down: the button shows the value, the list (while open) the choices; Esc / any other click closes it
                r.dd_id = id .. 'd'
                r.open = screen.dd == r.dd_id
                r.opts = {}
                for i, c in ipairs(d.choices) do
                    local oid = id .. 'o' .. i
                    r.opts[i] = {id = oid, label = c.label, on = i == index}
                    acts[oid] = function() set(c.value) end
                end
                acts[r.dd_id] = function() screen.dd = (R.dd_prev ~= r.dd_id) and r.dd_id or nil end
            elseif d.type == 'text' then
                r.kind, r.text, r.max_length = 'text', value, d.max_length or 64
                r.kbd_id = id .. 'k'
                R.pushed[id] = value
                txacts[id] = function(text)
                    if text == R.pushed[id] then return end
                    set(text:sub(1, d.max_length or 64))
                end
                acts[r.kbd_id] = function()
                    screen.osk = {mode = 'text', mod = e.id, def = d, label = d.label, text = value, max = d.max_length or 64}
                    screen.capture = nil
                    R.dirty = true
                end
            elseif d.type == 'color' then
                r.kind = 'color'
                local a, rr, g, b = bit.rshift(value, 24) % 256, bit.rshift(value, 16) % 256, bit.rshift(value, 8) % 256, value % 256
                r.a, r.r, r.g, r.b = a, rr, g, b
                r.css = ('#%02x%02x%02x%02x'):format(rr, g, b, a)
                local chans = {a = a, r = rr, g = g, b = b}
                for _, ch in ipairs({'a', 'r', 'g', 'b'}) do
                    local cid = id .. '_' .. ch
                    r[ch .. '_id'] = cid
                    R.pushed[cid] = chans[ch]
                    slacts[cid] = function(v)
                        if math.abs(v - R.pushed[cid]) < 1e-6 then return end
                        local cur = admin.get(e.id, d.id)
                        local c = {a = bit.rshift(cur, 24) % 256, r = bit.rshift(cur, 16) % 256, g = bit.rshift(cur, 8) % 256, b = cur % 256}
                        c[ch] = math.floor(v + 0.5)
                        set(ui.rgba(c.r, c.g, c.b, c.a))
                    end
                end
            end
            add(r)
        end
    end

    local function key_rows(e, m)
        local acts = R.acts
        local rows, section = {}, nil
        for _, k in ipairs(admin.keys(e.id)) do
            if not k.is_second then
                if k.section and k.section ~= section then rows[#rows + 1] = {head = k.section} end
                if k.section then section = k.section end
                rows[#rows + 1] = {key = k}
            end
        end
        for i, it in ipairs(rows) do
            if it.head then
                local r = row('head'); r.label = it.head
                m.rows[#m.rows + 1] = r
            else
                local k = it.key
                local second = k.second and admin.key_def(e.id, k.second) or nil
                local r = row('key')
                r.label = k.label
                local c1 = admin.conflicts(e.id, k.id)
                local c2 = second and admin.conflicts(e.id, second.id) or {}
                local names = {}
                for _, list in ipairs({c1, c2}) do
                    for _, c in ipairs(list) do names[#names + 1] = c.name .. ' / ' .. c.label end
                end
                r.conflict, r.conflict2 = #c1 > 0, #c2 > 0
                r.tip = (#names > 0 and ('Conflicts with: ' .. table.concat(names, ', ') .. '. ') or '') ..
                    'Default: ' .. format_binding(admin.default_binding(e.id, k.id))
                local function slot(def, prefix)
                    local capturing = screen.capture and screen.capture.mod == e.id and screen.capture.key == def.id
                    local text = capturing and 'Press a key...' or format_binding(admin.binding(e.id, def.id))
                    local sid, pid = 'k' .. i .. prefix, 'k' .. i .. prefix .. 'p'
                    acts[sid] = function()
                        screen.capture = {mod = e.id, key = def.id}
                        admin.capture_start()
                        R.dirty = true
                    end
                    acts[pid] = function()
                        screen.osk = {mode = 'key', mod = e.id, key = def.id, label = def.label, ctrl = false, shift = false, alt = false}
                        screen.capture = nil
                        R.dirty = true
                    end
                    return text, sid, pid, capturing and true or false
                end
                r.slot, r.slot_id, r.pick_id, r.capturing = slot(k, 'a')
                R.index.keys[k.id] = r
                if second then
                    r.has2 = true
                    r.slot2, r.slot2_id, r.pick2_id, r.capturing2 = slot(second, 'b')
                end
                r.changed = admin.binding(e.id, k.id) ~= admin.default_binding(e.id, k.id) or
                    (second and admin.binding(e.id, second.id) ~= admin.default_binding(e.id, second.id)) or false
                r.rst_id = 'k' .. i .. 'r'
                acts[r.rst_id] = function()
                    admin.set_binding(e.id, k.id, nil)
                    if second then admin.set_binding(e.id, second.id, nil) end
                    R.dirty = true
                end
                m.rows[#m.rows + 1] = r
            end
        end
    end

    local function osk_model(m)
        local o = screen.osk
        local acts = R.acts
        m.osk_text_mode = o.mode == 'text'
        m.osk_title = o.mode == 'text' and ('Typing: ' .. o.label) or ('Pick a key for: ' .. o.label)
        m.osk_text = o.text or ''
        m.osk_shift = o.shift and true or false
        m.osk_ctrl, m.osk_alt = o.ctrl and true or false, o.alt and true or false
        local function key_row(list, prefix, label)
            local out = {}
            for i, k in ipairs(list) do
                local id = prefix .. i
                out[#out + 1] = {id = id, label = label and label(k) or k}
                acts[id] = function()
                    if o.mode == 'text' then
                        if #o.text < o.max then
                            o.text = o.text .. ((o.shift and k:match('%a')) and k:upper() or k)
                            o.shift = false
                            admin.set(o.mod, o.def.id, o.text)
                        end
                    else
                        local b = (o.ctrl and 'ctrl+' or '') .. (o.shift and 'shift+' or '') .. (o.alt and 'alt+' or '') .. k
                        admin.set_binding(o.mod, o.key, b)
                        screen.osk = nil
                    end
                    R.dirty = true
                end
            end
            return out
        end
        local rows = {}
        for i, list in ipairs(OSK_TEXT) do
            rows[#rows + 1] = key_row(list, 'o' .. i .. '_', function(k) return (o.mode == 'text' and o.shift and k:match('%a')) and k:upper() or k end)
        end
        if o.mode ~= 'text' then
            rows[#rows + 1] = key_row(OSK_F, 'of_', function(k) return k:upper() end)
            rows[#rows + 1] = key_row(OSK_SPECIAL, 'os_', function(k) return OSK_SHORT[k] or (k:sub(1, 1):upper() .. k:sub(2, 3)) end)
        end
        m.osk_rows = rows
        local function toggle(name) return function() o[name] = not o[name]; R.dirty = true end end
        acts.osk_ctrl, acts.osk_alt = toggle('ctrl'), toggle('alt')
        acts.osk_shift, acts.osk_shift2 = toggle('shift'), toggle('shift')
        acts.osk_space = function()
            if #o.text < o.max then o.text = o.text .. ' '; admin.set(o.mod, o.def.id, o.text) end
            R.dirty = true
        end
        acts.osk_back = function() o.text = o.text:sub(1, -2); admin.set(o.mod, o.def.id, o.text); R.dirty = true end
        acts.osk_done = function() screen.osk = nil; R.dirty = true end
        acts.osk_none = function() admin.set_binding(o.mod, o.key, ''); screen.osk = nil; R.dirty = true end
        acts.osk_cancel = function() screen.osk = nil; R.dirty = true end
    end

    local function build()
        R.acts, R.slacts, R.txacts = {}, {}, {}
        R.index = {mods = {}, settings = {}, keys = {}, tabs = {}}
        local acts = R.acts
        local m = {open = true, view = 'list', message = screen.message or '', restart = screen.restart and true or false,
            count_text = '', mods = {}, mod_name = '', tabs = {}, tab = 'none', multi_tab = false, rows = {},
            osk_title = '', osk_text = '', osk_text_mode = true, osk_rows = {}, osk_ctrl = false, osk_shift = false, osk_alt = false}
        local list = admin.entries()
        m.count_text = ('%d installed'):format(#list - 1)
        if screen.view == 'list' then
            for i, e in ipairs(list) do
                local status, color = entry_status(e)
                local meta = ''
                if e.version ~= '' then meta = 'v' .. e.version end
                if e.author ~= '' then meta = meta .. (meta ~= '' and '  by ' or 'by ') .. e.author end
                local can_settings = e.state ~= 'invalid' and (e.has_settings or e.has_page)
                local mr = {id = e.id, name = e.name, meta = meta, desc = e.description ~= '' and e.description or '(no description)',
                    status = status or '', st_red = color == RED, st_amber = color == AMBER, st_grey = color == DIM,
                    bad = e.state == 'error' or e.state == 'invalid', dim = e.state == 'off' or e.state == 'blocked',
                    core = e.builtin and true or false, can_toggle = not e.builtin and e.state ~= 'invalid', on = e.enabled and true or false,
                    en_id = 'me' .. i, set_id = 'ms' .. i, keys_id = 'mk' .. i,
                    en_tip = e.enabled and 'Switch this mod off' or 'Switch this mod on',
                    no_settings = not can_settings, no_keys = not e.has_keys}
                m.mods[#m.mods + 1] = mr
                R.index.mods[e.id] = {en = mr.en_id, settings = mr.set_id, keys = mr.keys_id}
                local sid = e.id
                acts[mr.en_id] = function()
                    local cur = find_entry(sid)
                    local ok, why = admin.set_enabled(sid, not (cur and cur.enabled))
                    screen.message = (not ok) and why or nil
                    R.dirty = true
                end
                if can_settings then
                    acts[mr.set_id] = function()
                        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, (e.has_settings and 'settings' or 'page'), 1
                        stop_input()
                        R.dirty, R.reset_scroll = true, true
                    end
                end
                if e.has_keys then
                    acts[mr.keys_id] = function()
                        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, 'keys', 1
                        stop_input()
                        R.dirty, R.reset_scroll = true, true
                    end
                end
            end
        else
            local e = find_entry(screen.mod)
            if not e then
                screen.view = 'list'
                R.dirty = true
                return build()
            end
            m.view, m.mod_name = 'mod', e.name
            local tabs = {}
            if e.has_settings then tabs[#tabs + 1] = {'settings', 'Settings'} end
            if e.has_keys then tabs[#tabs + 1] = {'keys', 'Keys'} end
            if e.has_page then tabs[#tabs + 1] = {'page', 'More'} end
            local present = false
            for _, t in ipairs(tabs) do if t[1] == screen.tab then present = true end end
            if not present and tabs[1] then screen.tab = tabs[1][1] end
            for _, t in ipairs(tabs) do
                local id = 'tab_' .. t[1]
                m.tabs[#m.tabs + 1] = {id = id, label = t[2], on = screen.tab == t[1]}
                R.index.tabs[t[1]] = id
                acts[id] = function()
                    if screen.tab ~= t[1] then
                        screen.tab, screen.tab_page = t[1], 1
                        stop_input()
                        R.dirty, R.reset_scroll = true, true
                    end
                end
            end
            m.tab = #tabs == 0 and 'none' or screen.tab
            m.multi_tab = #tabs > 1
            R.tab_order = {}
            for i, t in ipairs(tabs) do R.tab_order[i] = t[1] end
            if screen.tab == 'settings' then setting_rows(e, m)
            elseif screen.tab == 'keys' then key_rows(e, m) end
            acts.back = function() screen.view = 'list'; stop_input(); R.dirty, R.reset_scroll = true, true end
            acts.reset_all = function() admin.reset(e.id); R.dirty = true end
            if screen.osk then m.view = 'osk'; osk_model(m) end
        end
        acts.close = function() close_screen() end
        return m
    end

    -- the document, loaded on the first need; nil (and the gear.ui screen) while it cannot
    local function document()
        if R.doc then return R.doc end
        local now = gear.time()
        if now < R.retry or not ui.rml or not admin.markup then return nil end
        local markup = admin.markup()
        if not markup then R.retry = now + 60 return nil end
        R.dirty = true
        local m = build()
        R.doc, R.err = ui.rml.load_string(markup, {model = m})
        if not R.doc then
            R.retry = now + 5
            if not R.logged then R.logged = true; gear.log('mod manager: RmlUi screen unavailable (' .. tostring(R.err) .. '): using the basic screen') end
            return nil
        end
        for k, v in pairs(m) do R.last[k] = ser(v) end
        local doc = R.doc
        doc:on('.act', 'click', function(e)
            local f = R.acts[tostring(e.current)]
            R.dd_prev, screen.dd = screen.dd, nil       -- a click anywhere else closes an open drop-down (its own button toggles it)
            if f then f() end
            R.dirty = true
        end)
        doc:on('.sl', 'change', function(e)
            local f = R.slacts[tostring(e.current)]
            local v = tonumber(e.params and e.params.value)
            -- only the player's own move counts: RmlUi also reports a change when the model sets the slider (rows reused for another setting
            -- are clamped to the old range for a moment), which used to write the range's minimum into the setting
            if f and v and e.user then f(v) end
        end)
        doc:on('.tx', 'change', function(e)
            local f = R.txacts[tostring(e.current)]
            if f and e.params and e.params.value then f(tostring(e.params.value)) end
        end)
        return doc
    end

    -- For the self test (src/gear/examples/modmanagertest): gear.emit('mods.query', out) fills `out` with where the screen is, the ids of the elements of the
    -- mods, settings, keys and tabs shown (R.index; an element is found by '#id'), rect(selector) -> {x, y, w, h}, reveal(selector) (scrolls it into the
    -- visible part of the list) and press(id) (what a click on that element does).
    gear.on('mods.query', function(out)
        out.open, out.view, out.mod, out.tab = screen.open, screen.view, screen.mod, screen.tab
        out.paste_shown = paste_shown
        out.index = R.index
        out.tab_order = R.tab_order
        out.capturing = screen.capture ~= nil
        out.osk = screen.osk ~= nil
        out.rect = function(sel)
            local e = R.doc and R.doc:query(sel)
            return e and e:rect() or nil
        end
        out.reveal = function(sel)
            local doc = R.doc
            local e = doc and doc:query(sel)
            local er = e and e:rect()
            if not er then return nil end
            for _, box in ipairs(doc:query_all('.bodybox')) do
                local br = box:rect()
                if br and br.w > 0 then
                    local top, _, client = box:scroll()
                    if er.y < br.y then box:scroll(top - (br.y - er.y) - 8)
                    elseif er.y + er.h > br.y + client then box:scroll(top + (er.y + er.h - br.y - client) + 8) end
                    break
                end
            end
            return e:rect()
        end
        out.scroll_top = function()
            for _, box in ipairs(R.doc and R.doc:query_all('.bodybox') or {}) do
                local br = box:rect()
                if br and br.w > 0 then return (box:scroll()) end
            end
        end
        out.attr = function(sel, name)
            local doc = R.doc
            return doc and doc:attr(sel, name)
        end
        out.text = function(sel)
            local doc = R.doc
            return doc and doc:text(sel)
        end
        out.press = function(id)
            local f = R.acts[id]
            if f then f() return true end
        end
    end)

    -- true when the document drew the screen (the caller then has nothing to draw)
    function draw_rml_screen(back, escape)
        local doc = document()
        if not doc then return false end
        local l, t, r, b = ui.screen()
        local blocker = ui.region('blocker', l, t, r - l, b - t, {layer = 'top', nav = false})
        if blocker.pressed then stop_input() end
        if back then
            -- The controller's B: out of the keyboard, out of a mod's screen, then out of the manager.
            if screen.dd then screen.dd = nil; R.dirty = true
            elseif screen.osk then screen.osk = nil
            elseif screen.capture then stop_input()
            elseif screen.view ~= 'list' then screen.view = 'list'; R.reset_scroll = true
            else close_screen() return true end
            R.dirty = true
        elseif escape then
            if screen.dd then screen.dd = nil; R.dirty = true
            elseif screen.osk or screen.capture then stop_input(); R.dirty = true else close_screen(true) return true end
        end
        -- a key being rebound: the next key pressed is the binding
        local cap = screen.capture
        if cap then
            local bnd = admin.capture()
            if bnd then
                if bnd == 'backspace' or bnd == 'delete' then bnd = '' end
                admin.set_binding(cap.mod, cap.key, bnd)
                screen.capture = nil
                R.dirty = true
            end
        end
        -- LB / RB: the previous / next tab of a mod's screen, a page up / down of the list
        local page = ui.page_pressed()
        if page ~= 0 and not screen.osk then
            if screen.view == 'mod' and R.tab_order then
                local at = 1
                for i, name in ipairs(R.tab_order) do if name == screen.tab then at = i end end
                local to = math.max(1, math.min(#R.tab_order, at + page))
                if to ~= at then
                    screen.tab, screen.tab_page = R.tab_order[to], 1
                    stop_input()
                    R.dirty, R.reset_scroll = true, true
                end
            elseif screen.view == 'list' then
                for _, box in ipairs(doc:query_all('.bodybox')) do
                    local top, _, client = box:scroll()
                    if top then box:scroll(top + page * math.floor(client * 0.9)) end
                end
            end
        end
        local now = gear.time()
        if R.dirty or now - R.built_at > 0.25 then
            R.dirty, R.built_at = false, now
            local m = build()
            for k, v in pairs(m) do
                local s = ser(v)
                if R.last[k] ~= s then
                    doc.model[k] = v
                    R.last[k] = s
                end
            end
        end
        if R.reset_scroll then
            R.reset_scroll = false
            for _, e in ipairs(doc:query_all('.bodybox')) do e:scroll('start') end
        end
        ui.capture_keys(true)
        -- Centred on the whole screen (the document's own left/top are for the 1024 x 768 layout; a layout that is
        -- larger or offset, as the game's own in ReBorn, has the screen's edges elsewhere).
        local cx, cy = math.floor((l + r - 800) / 2 + 0.5), math.floor((t + b - 680) / 2 + 0.5)
        if R.win_x ~= cx or R.win_y ~= cy then
            R.win_x, R.win_y = cx, cy
            doc:style('#win', 'left', ('%dpx'):format(cx))
            doc:style('#win', 'top', ('%dpx'):format(cy))
        end
        doc:draw({layer = 'top'})
        if screen.view == 'mod' and screen.tab == 'page' and not screen.osk then
            local area = doc:query('#more')
            local rect = area and area:rect()
            if rect and rect.w > 0 then admin.draw_page(screen.mod, rect.x, rect.y, rect.w, rect.h) end
        end
        return true
    end
end

---------------------------------------------------------------------------------------------------------------
-- Each frame
---------------------------------------------------------------------------------------------------------------
-- The item of the game menu in a native menu (gear.menu); the button below stands in for it otherwise.
gear.menu.add_item('esc', {id = 'mods', label = 'Mods', order = 900, tooltip = 'Mod manager', on_click = function() open_screen() end})

-- The button as an RmlUi element of the theme (class game-button: the game's own button, its font, the yellow hover, a pad prompt): one document per place.
-- Without the theme mod (or RmlUi) the document cannot load and the button is the old gear.ui one, so nothing depends on it.
local rml_buttons = {}
-- The button is placed by its real width: its centre goes to `cx` (nil: its left edge to `left`), its top to `y`.
local function rml_button(key, cx, left, y, select_key)
    local entry = rml_buttons[key]
    if entry == nil then
        entry = {retry = 0}
        rml_buttons[key] = entry
    end
    if entry.doc then return entry.doc end
    local now = gear.time()
    if entry.failed and now < entry.retry then return nil end
    if not ui.rml then entry.failed, entry.retry = true, now + 5 return nil end
    -- The pad prompts: while the button has the pad's focus the confirm button (A); else, where Select / "-" opens the screen (the escape menu), that
    -- button; where nothing does, none.
    local prompts = '<img class="padkey conf" src="gear:pad/a"/>' .. (select_key and '<img class="padkey sel" src="gear:pad/back"/>' or '')
    local doc = ui.rml.load_string('<rml><head><style>'
        .. 'body.pad button .padkey.conf { display: none; } body.pad button:focus .padkey.conf { display: inline-block; } '
        .. 'body.pad button .padkey.sel { display: inline-block; } body.pad button:focus .padkey.sel { display: none; } '
        .. '</style></head><body><button id="b" class="game-button" style="position: absolute; left: -2000px; top: 0px; margin: 0; width: 104px;">'
        .. prompts .. 'Mods</button></body></rml>')
    if not doc then entry.failed, entry.retry = true, now + 5 return nil end
    doc:on('#b', 'click', function() open_screen() end)
    entry.doc = doc
    return doc
end

local function place_rml_button(doc, entry, cx, left, y)
    local r = doc:query('#b'):rect()
    if not r or r.w <= 0 then return end
    local x = cx and (cx - r.w / 2) or left
    if entry.x ~= x or entry.y ~= y then
        entry.x, entry.y = x, y
        doc:style('#b', 'left', ('%.2fpx'):format(x))
        doc:style('#b', 'top', ('%.2fpx'):format(y))
    end
end

local function escape_menu_button(state)
    if state.mode == 'menus' then
        local doc = rml_button('menu', nil, nil, nil, true)
        if doc then
            place_rml_button(doc, rml_buttons.menu, nil, 16, 718)
            doc:draw({layer = 'top'})
        elseif ui.button('menu_button', 16, 724, 90, 26, 'Mods', {layer = 'top', tooltip = 'Mod manager'}) then
            open_screen()
        end
        -- Select / "-" on the pad opens the screen while the main menu is shown
        if ui.select_pressed() then open_screen() end
    elseif state.escape_menu and not state.message_box and not gear.menu.available('esc') then
        -- With a native menu (a mod that draws the game menu itself) the "Mods" item below is part of it; else this button.
        local l, t, r, b = ui.screen()
        local x = (l + r) / 2 - 60      -- below the escape menu's window (centered in the layout)
        local doc = rml_button('esc', nil, nil, nil, true)
        if doc then
            -- Centred under the game menu's window (the one Gear reports), as the old button was; the gap below the frame is the old one's.
            local win = ui.window('UI_WND_ESCMENU')
            local cx = win and (win.x + win.width / 2) or (l + r) / 2
            local by = win and win.height > 0 and (win.y + win.height + 7) or 507      -- 507 is under the window of the 1024 x 768 layout
            place_rml_button(doc, rml_buttons.esc, cx, nil, by)
            doc:draw({layer = 'top'})
        elseif ui.button('esc_button', x, 508, 120, 26, 'Mods', {layer = 'top', tooltip = 'Mod manager'}) then
            open_screen()
        end
        -- Select / "-" on the pad opens the screen while the escape menu is up (the game menu items have no such button)
        if ui.select_pressed() then open_screen() end
    end
end

gear.on('draw', function(state)
    -- (a Select press only counts while the escape menu or the main menu is up: older ones are dropped)
    if not ((state.mode == 'menus' or (state.escape_menu and not state.message_box)) and not screen.open) then while ui.select_pressed() do end end
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
    if draw_rml_screen(back, escape) then return end
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

-- gear.emit('mods.open', {mod = 'autosave', tab = 'settings'}): opens the screen on a mod's own screen (tab 'settings', 'keys' or 'page'; the first it has when
-- tab is left out), or on the list when no mod is named. For mods that link to their own settings, and for the screenshot tools.
gear.on('mods.open', function(t)
    t = type(t) == 'table' and t or {}
    if not screen.open then open_screen() end
    local e = t.mod and find_entry(t.mod)
    stop_input()
    screen.osk = nil
    if not e or e.state == 'invalid' then
        screen.view = 'list'
        return
    end
    local tab = t.tab
    if tab ~= 'settings' and tab ~= 'keys' and tab ~= 'page' then tab = e.has_settings and 'settings' or (e.has_keys and 'keys' or 'page') end
    screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', e.id, tab, 1
end)

gear.on('unload', function() ui.nav_modal(false); admin.modal(false) end)

---------------------------------------------------------------------------------------------------------------
-- A Paste button over the game's own text boxes. While one has the keyboard focus (the savegame name, the character name,
-- the chat line, a search field ...), a small button sits inside it at its right end: a click types what is on the clipboard
-- into the box, as if it had been typed (the box applies its own length limit). The mouse only: the button is not a stop of the
-- controller's D-pad (the pad has the game's own on-screen keyboard). Ctrl+V pastes too.
---------------------------------------------------------------------------------------------------------------
local PASTE = {w = 46, pad = 3, icon_w = 26}
local paste_pending = false
local function paste_clipboard()
    local text = gear.clipboard.get()
    if text and text ~= '' then
        text = text:gsub('[\r\n\t]+', ' ')
        admin.type_text(text)
    end
end
-- The paste icon (Gear's icon font, the game's grey): a small RmlUi document that follows the text box; without RmlUi the old text button.
local paste_doc
local function paste_icon()
    if paste_doc ~= nil then return paste_doc or nil end
    paste_doc = false
    if ui.rml and ui.rml.load_string then
        local d = ui.rml.load_string([[<rml><head><style>
            #pi { position: absolute; left: 0px; top: 0px; width: 26px; height: 22px; margin: 0; padding: 0; border: 0; background-color: transparent; text-align: center; line-height: 22px; font-size: 17px; color: #a6a39c; cursor: pointer; }
            #pi:hover, #pi:focus { color: #ece8dc; background-color: #ffffff1c; border: 0; transform: none; }
        </style></head><body><button id="pi" data-no-padkey="" title="Paste"><span class="icon icon-content-paste"></span></button></body></rml>]], {theme = false})
        paste_doc = d or false
        if d then d:on('#pi', 'click', function() paste_clipboard() end) end
    end
    return paste_doc or nil
end
gear.on('draw', function(state)
    if type(state) ~= 'table' or not (state.mode == 'game' or state.mode == 'menus') then return end
    paste_shown = false
    if screen.open then return end
    local x, y, w, h = admin.text_focus()
    -- Ctrl+V into the focused box does what the Paste button does (not with AltGr, which is Ctrl+Alt; the key is polled every frame for its edge).
    -- The characters are typed once Ctrl is up: the game's box ignores characters while it sees Ctrl held.
    local v_pressed = ui.key_pressed('v')
    local ctrl = ui.key_down('ctrl')
    if v_pressed and ctrl and not ui.key_down('alt') and x then paste_pending = true end
    if not x then paste_pending = false return end
    if paste_pending and not ctrl then
        paste_pending = false
        paste_clipboard()
    end
    paste_shown = true
    local bh = math.max(12, math.min(h - 6, 22))
    local icon = paste_icon()
    if icon then
        local bx, by = x + w - PASTE.icon_w - PASTE.pad, y + math.floor((h - bh) / 2)
        icon:style('#pi', 'left', ('%dpx'):format(bx))
        icon:style('#pi', 'top', ('%dpx'):format(by))
        icon:style('#pi', 'height', ('%dpx'):format(bh))
        icon:style('#pi', 'line-height', ('%dpx'):format(bh))
        icon:draw({layer = 'top'})
        return
    end
    local bx, by = x + w - PASTE.w - PASTE.pad, y + math.floor((h - bh) / 2)
    if ui.button('paste_overlay', bx, by, PASTE.w, bh, 'Paste', {layer = 'top', nav = false, tooltip = 'Type the clipboard into this box'}) then
        paste_clipboard()
    end
end)
