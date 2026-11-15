-- Sacred Gear's mod manager: a screen listing the installed mods (switch on and off, their settings and key binds),
-- drawn with gear.ui widgets. It is a mod of its own that ships in SacredGear.dll (the kernel loads it); it gets the
-- extra global `admin` the kernel gives only to it. Opened with its hotkey (F11, rebindable in this screen) or the
-- "Mods" button beside the game's escape menu.

local ui = gear.ui
local C = ui.COLORS

local ROW_H, ROW_STEP = 58, 62   -- a mod's row in the list and the distance between rows
local SETTING_ROWS = 9      -- rows of settings per page (section headers count)
local KEY_ROWS = 7          -- key binds per page
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
        w = ui.text_size(text, opts) or (#text * 6)
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
end

local function open_screen()
    screen.holding, screen.up_at = false, nil
    screen.open = true
    screen.view, screen.message = 'list', nil
    stop_input()
    ui.show_panel('main', true)
    admin.modal(true)
end

-- `by_escape`: the keyboard stays kept from the game until the key is up (and a moment longer): the game's message
-- loop may still hold the Esc that closed the screen, and would open its escape menu with it.
local function close_screen(by_escape)
    screen.open = false
    stop_input()
    ui.show_panel('main', false)
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
    local cx = x + w / 2
    if ui.button(id .. '<', cx - 90, y, 30, 22, '<', {layer = 'top', enabled = page > 1, tooltip = 'Previous page'}) then
        page = page - 1
        stop_input()
    end
    ui.text(('Page %d / %d'):format(page, pages), cx - 55, y, {width = 110, height = 22, align = 'center',
        valign = 'middle', color = C.text, layer = 'top'})
    if ui.button(id .. '>', cx + 60, y, 30, 22, '>', {layer = 'top', enabled = page < pages, tooltip = 'Next page'}) then
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
    local h = ROW_H
    local bad = e.state == 'error' or e.state == 'invalid'
    ui.rect(x, y, w, h, bad and 0x60401818 or 0x50181824, LAYER)
    ui.frame(x, y, w, h, bad and 0xFF804040 or C.edge_dim, 1, LAYER)
    local sid = e.id
    -- switch
    local dim = e.state == 'off' or e.state == 'blocked'
    if e.builtin or e.state == 'invalid' then
        ui.text(e.builtin and 'Core' or '', x + 8, y + 8, {width = 44, height = 20, align = 'center', valign = 'middle',
            color = DIM, layer = 'top'})
    else
        local on = e.enabled
        if ui.button('en:' .. sid, x + 8, y + 8, 44, 20, on and 'On' or 'Off',
            {layer = 'top', color = on and GREEN or RED, tooltip = on and 'Switch this mod off' or 'Switch this mod on'}) then
            local ok, why = admin.set_enabled(sid, not on)
            if not ok then screen.message = why end
        end
    end
    -- name, version, author
    local name_opts = {size = 14, bold = true}
    ui.text(fit(e.name, 280, name_opts), x + 62, y + 5, {size = 14, bold = true, color = dim and DIM or C.accent, layer = 'top'})
    local nx = x + 62 + math.min(text_w(e.name, name_opts), 280) + 8
    local meta = ''
    if e.version ~= '' then meta = 'v' .. e.version end
    if e.author ~= '' then meta = meta .. (meta ~= '' and '  by ' or 'by ') .. e.author end
    ui.text(fit(meta, x + w - 190 - nx, {size = 11}), nx, y + 8, {size = 11, color = DIM, layer = 'top'})
    -- description, status
    local text_width = w - 62 - 186
    ui.text(fit(e.description ~= '' and e.description or '(no description)', text_width), x + 62, y + 24,
        {color = dim and DIM or C.text, layer = 'top'})
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
        ui.text(fit(status, w - 62 - 8), x + 62, y + 40, {size = 11, color = color, layer = 'top'})
    end
    -- buttons
    local can_settings = e.state ~= 'invalid' and (e.has_settings or e.has_page)
    if ui.button('set:' .. sid, x + w - 176, y + 8, 80, 22, 'Settings',
        {layer = 'top', enabled = can_settings, tooltip = can_settings and nil or 'This mod has no settings'}) then
        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, (e.has_settings and 'settings' or 'page'), 1
        stop_input()
    end
    if ui.button('keys:' .. sid, x + w - 90, y + 8, 80, 22, 'Keys',
        {layer = 'top', enabled = e.has_keys, tooltip = e.has_keys and nil or 'This mod declares no keys'}) then
        screen.view, screen.mod, screen.tab, screen.tab_page = 'mod', sid, 'keys', 1
        stop_input()
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
    local fy = footer_y(p)
    screen.page = paging('lp', x, fy, w, screen.page, pages)
    ui.text('Saved in mods\\gear_mods.json', x, fy, {height = FOOTER_ITEM_H, valign = 'middle', size = 11, color = DIM, layer = 'top'})
    if ui.button('close', x + w - 80, fy, 80, FOOTER_ITEM_H, 'Close', LAYER) then close_screen() end
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

local function setting_row(m, d, x, y, w)
    local id = 's:' .. m .. ':' .. d.id
    local value = admin.get(m, d.id)
    local label = d.label .. (d.restart and '  (after restart)' or '')
    ui.label(id .. '#l', label, x + 6, y, {tooltip = d.tooltip, height = 24, valign = 'middle', layer = 'top'})
    local cx = x + 290
    local changed, nv = false, value
    if d.type == 'bool' then
        changed, nv = ui.checkbox(id, cx, y + 4, value and 'On' or 'Off', value, LAYER)
    elseif d.type == 'number' then
        changed, nv = ui.slider(id, cx, y + 4, 200, d.min, d.max, value, {step = d.step, layer = 'top'})
    elseif d.type == 'choice' then
        local labels, index = {}, 1
        for i, c in ipairs(d.choices) do
            labels[i] = c.label
            if c.value == value then index = i end
        end
        local ch, i = ui.dropdown(id, cx, y + 3, 220, labels, index, LAYER)
        if ch then changed, nv = true, d.choices[i].value end
    elseif d.type == 'text' then
        local focused = screen.edit == id
        local r = ui.region(id, cx, y + 2, 250, 20, LAYER)
        ui.rect(cx, y + 2, 250, 20, focused and 0xF0202030 or C.bg, LAYER)
        ui.frame(cx, y + 2, 250, 20, (focused or r.hover) and C.edge or C.edge_dim, 1, LAYER)
        if r.clicked and not focused then
            screen.edit, screen.edit_text, screen.capture = id, value, nil
            focused = true
        end
        local shown = focused and screen.edit_text or value
        local blink = focused and math.floor(gear.time() * 2) % 2 == 0 and '|' or ''
        ui.text(fit(shown, 238) .. blink, cx + 5, y + 2, {height = 20, valign = 'middle', layer = 'top'})
        if focused then
            if ui.key_pressed('enter') or ui.key_pressed('tab') then
                screen.edit = nil
            elseif process_typing(d.max_length) then
                changed, nv = true, screen.edit_text
            end
        end
    elseif d.type == 'color' then
        local a, r, g, b = bit.rshift(value, 24) % 256, bit.rshift(value, 16) % 256, bit.rshift(value, 8) % 256, value % 256
        ui.rect(cx, y + 3, 22, 18, ui.rgba(255, 255, 255, 255), LAYER)
        ui.rect(cx, y + 3, 22, 18, value, LAYER)
        ui.frame(cx, y + 3, 22, 18, C.edge, 1, LAYER)
        local chans = {{'a', 'Alpha', a}, {'r', 'Red', r}, {'g', 'Green', g}, {'b', 'Blue', b}}
        for i, c in ipairs(chans) do
            local ch, v = ui.slider(id .. c[1], cx + 30 + (i - 1) * 62, y + 4, 54, 0, 255, c[3],
                {step = 1, format = false, layer = 'top', tooltip = c[2] .. ' ' .. c[3]})
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
        if ui.button(id .. '#reset', x + w - 54, y + 2, 50, 20, 'Reset', {layer = 'top', tooltip = 'Back to the default'}) then
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
    -- Dropdowns last, from the bottom row up, so an open list lies over (and takes the clicks of) the rows below it.
    local order = {}
    for i, item in ipairs(items) do if item.type ~= 'choice' then order[#order + 1] = i end end
    for i = #items, 1, -1 do if items[i].type == 'choice' then order[#order + 1] = i end end
    for _, i in ipairs(order) do
        local item = items[i]
        local ry = y + (i - 1) * 32
        if item.header then
            ui.text(item.header, x + 2, ry + 2, {size = 13, bold = true, color = C.accent, height = 24, valign = 'middle', layer = 'top'})
            ui.rect(x + 2 + text_w(item.header, {size = 13, bold = true}) + 8, ry + 13, w - text_w(item.header, {size = 13, bold = true}) - 14, 1,
                C.edge_dim, LAYER)
        else
            setting_row(e.id, item, x, ry, w)
        end
    end
    local fy = footer_y(p)
    if ui.button('reset_all', x, fy, 130, 22, 'Reset to defaults', {layer = 'top', tooltip = 'Every setting of this mod back to its default'}) then
        admin.reset(e.id)
        if #defs > 0 then screen.restart = screen.restart end
    end
    if #pages > 1 then
        screen.tab_page = paging('sp', x + 80, fy, w - 160, screen.tab_page, #pages)
    end
    if screen.restart then
        ui.text('Some changes apply after a restart', x + w - 230, fy, {width = 230, height = 22, valign = 'middle',
            align = 'right', size = 11, color = AMBER, layer = 'top'})
    end
end

---------------------------------------------------------------------------------------------------------------
-- Key binds of one mod
---------------------------------------------------------------------------------------------------------------
local function draw_keys(p, e)
    local x, y, w = p.content_x, p.content_y + 34, p.content_w
    local keys = admin.keys(e.id)
    local pages = math.max(1, math.ceil(#keys / KEY_ROWS))
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
        local k = keys[first + i]
        if not k then break end
        local ry = y + (i - 1) * 44
        local id = 'k:' .. e.id .. ':' .. k.id
        local binding = admin.binding(e.id, k.id)
        local conflicts = admin.conflicts(e.id, k.id)
        ui.label(id .. '#l', k.label, x + 6, ry, {height = 24, valign = 'middle', layer = 'top',
            tooltip = 'Default: ' .. format_binding(admin.default_binding(e.id, k.id))})
        local capturing = screen.capture and screen.capture.mod == e.id and screen.capture.key == k.id
        local text = capturing and 'Press a key...' or format_binding(binding)
        if ui.button(id, x + 290, ry + 1, 150, 22, text,
            {layer = 'top', color = capturing and C.accent or (#conflicts > 0 and AMBER or nil),
             tooltip = 'Click, then press the new key. Esc cancels, Backspace unbinds.'}) then
            screen.capture = {mod = e.id, key = k.id}
            screen.edit = nil
            admin.capture_start()
        end
        if binding ~= admin.default_binding(e.id, k.id) then
            if ui.button(id .. '#reset', x + 448, ry + 1, 60, 22, 'Default', {layer = 'top', tooltip = 'Back to the default key'}) then
                admin.set_binding(e.id, k.id, nil)
            end
        end
        if #conflicts > 0 then
            local names = {}
            for _, c in ipairs(conflicts) do names[#names + 1] = c.name .. ' / ' .. c.label end
            ui.text(fit('Conflicts with: ' .. table.concat(names, ', '), w - 12), x + 6, ry + 25,
                {size = 11, color = AMBER, layer = 'top'})
        end
    end
    local fy = footer_y(p)
    ui.text('Click a key, then press the new one. Esc cancels, Backspace unbinds.', x, fy,
        {height = 22, valign = 'middle', size = 11, color = DIM, layer = 'top'})
    if pages > 1 then screen.tab_page = paging('kp', x + w - 280, fy, 200, screen.tab_page, pages) end
end

---------------------------------------------------------------------------------------------------------------
-- A mod's screen: tabs
---------------------------------------------------------------------------------------------------------------
local function draw_mod(p)
    local e = find_entry(screen.mod)
    if not e then screen.view = 'list'; return end
    local x, y, w = p.content_x, p.content_y, p.content_w
    if ui.button('back', x, y, 70, 22, '< Back', LAYER) then
        screen.view = 'list'
        stop_input()
        return
    end
    ui.text(fit(e.name, 220, {size = 14, bold = true}), x + 80, y, {size = 14, bold = true, color = C.accent, height = 22,
        valign = 'middle', layer = 'top'})
    local tabs = {}
    if e.has_settings then tabs[#tabs + 1] = {'settings', 'Settings'} end
    if e.has_keys then tabs[#tabs + 1] = {'keys', 'Keys'} end
    if e.has_page then tabs[#tabs + 1] = {'page', 'More'} end
    local present = false
    for _, t in ipairs(tabs) do if t[1] == screen.tab then present = true end end
    if not present and tabs[1] then screen.tab = tabs[1][1] end
    for i, t in ipairs(tabs) do
        local tx = x + w - (#tabs - i + 1) * 84
        local active = screen.tab == t[1]
        if ui.button('tab:' .. t[1], tx, y, 80, 22, t[2], {layer = 'top', color = active and C.accent or nil}) and not active then
            screen.tab, screen.tab_page = t[1], 1
            stop_input()
        end
        if active then ui.rect(tx, y + 22, 80, 2, C.accent, LAYER) end
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
    if not screen.open then
        escape_menu_button(state)
        return
    end
    local escape = ui.key_pressed('escape')
    local l, t, r, b = ui.screen()
    local blocker = ui.region('blocker', l, t, r - l, b - t, LAYER)
    if blocker.pressed then stop_input() end
    local H = math.max(300, math.min(H_MAX, b - t - 16))
    local px, py = (l + r - W) / 2, math.max(t + 8, (t + b - H) / 2)
    local p = ui.panel('main', 'Mods', px, py, W, H, {layer = 'top', closable = true})
    if not p.open then
        close_screen()
        return
    end
    if escape then
        if screen.capture or screen.edit then stop_input() else close_screen(true) return end
    end
    if screen.view == 'list' then
        draw_list(p)
    else
        draw_mod(p)
    end
end)

gear.keys.on('open', toggle_screen)

gear.on('unload', function() admin.modal(false) end)
