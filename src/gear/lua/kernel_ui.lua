---------------------------------------------------------------------------------------------------------------
-- UI drawing (gear.ui). Immediate mode: what a mod draws during a tick (its 'frame' and 'draw' handlers, timers,
-- tasks) is shown until the next tick, then replaced; the native side draws it inside the game's UI pass. Positions
-- are the UI's 1024x768 layout. Colors are 0xAARRGGBB numbers (gear.ui.rgba).
---------------------------------------------------------------------------------------------------------------
local UI_LAYERS = {world = 0, hud = 1, top = 2}
local UI_FITS = {stretch = 0, contain = 1, fit = 1, cover = 2, fill = 2}
UI_FITS[1] = {h = 4, v = 8, hv = 12, vh = 12}          -- [1]: added to the fit for native.ui_image (a flip)
local UI_ALIGN = {left = 0, center = 1, right = 4}   -- the fonts' flags
local UI_VALIGN = {top = 0, middle = 1, bottom = 2, baseline = 3}
local UI_MAX_PER_MOD = 2000     -- draw commands per mod and tick
local ui_counts = {}            -- mod -> commands this tick
local ui_slots = nil            -- window name (upper case) -> slot, this tick

local function ui_color(c, default)
    if c == nil then return default end
    if type(c) == 'number' then return c % 4294967296 end
    if type(c) == 'table' then
        local function b(v, d) v = v or d; return math.max(0, math.min(255, math.floor(v + 0.5))) end
        return ((b(c[4] or c.a, 255) * 256 + b(c[1] or c.r, 0)) * 256 + b(c[2] or c.g, 0)) * 256 + b(c[3] or c.b, 0)
    end
    error('a color is 0xAARRGGBB or {r, g, b, a}', 3)
end

local function ui_window_slot(name)
    if not ui_slots then
        ui_slots = {}
        for _, w in ipairs(native.ui_windows()) do ui_slots[w.name:upper()] = w.slot end
    end
    return ui_slots[name:upper()]
end

-- The layer and window slot of opts; nil if the window does not exist (then nothing is drawn).
local function ui_target(opts)
    local layer = UI_LAYERS[opts.layer or 'hud']
    if not layer then error('layer is "world", "hud" or "top"', 3) end
    local slot = -1
    if opts.window ~= nil then
        if type(opts.window) ~= 'string' then error('window is a control name ("UI_WND_INVENTORY")', 3) end
        slot = ui_window_slot(opts.window)
        if not slot then return nil end
    end
    return layer, slot
end

local function ui_count(mod, count)
    local n = (ui_counts[mod] or 0) + (count or 1)
    if n > UI_MAX_PER_MOD then error('too many draw commands this frame (' .. UI_MAX_PER_MOD .. ')', 3) end
    ui_counts[mod] = n
end

-- Input for widgets. Regions (gear.ui.region and every widget) are rectangles that take the mouse: a press over the
-- topmost one is taken from the game and reported with the next tick. Hover is decided against the previous
-- frame's regions (the topmost under the cursor), so a dropdown's open list hides what lies below it.
local ui_handles = {}       -- key (mod id .. '\0' .. widget id) -> handle
local ui_handle_key = {}    -- handle -> key
local ui_next_handle = 1
local ui_regions = {n = 0}  -- this tick's regions, in order: {handle, layer, x, y, w, h, id, mod}; entries are reused (n = how many are live)
local ui_prev_regions = {n = 0}
local ui_input = {}         -- handle -> {pressed, released, clicked} of this tick (tables pooled)
local ui_active = nil       -- the handle whose press has no release yet
local ui_hot = nil          -- the topmost region under the cursor
local ui_mouse = {x = 0, y = 0}
local ui_tick_state = nil   -- native.ui_state() of this tick
local ui_tooltip = nil      -- {text, mod} to draw at the end of the tick
local ui_hover_since = {}   -- handle -> time the cursor came over it
local ui_dropdown = nil     -- the open dropdown's handle
local ui_keys = {}          -- mod -> vk -> held at the last query
local key_capture = {}      -- mod -> true: the mod keeps the keyboard from the game (ui.capture_keys)
local ui_panel_list = {}    -- the panels of all mods drawn this tick, in order: {key, title, x, y, w, h, ...}
local ui_panel_prev = {}    -- the last tick's
-- The controller (SacredBild's D-pad): the widgets' rectangles are reported as stops each tick; the pad's events
-- (slider nudges, B, the mod menu button) come back.
local ui_nav = {n = 0}      -- this tick's stops: {mod, x, y, w, h, layer, kind, ax, ay, drop}; entries are reused (n = how many are live)
local ui_nav_modal = nil    -- the mod whose screen takes the pad alone (ui.nav_modal)
local ui_window_open = false    -- a closable panel is up this tick
local ui_pad = {nudge = 0, back = 0, menu = 0, page = 0, select = 0}
local ui_menu_items = {}    -- mod -> {{id, label, fn}} (ui.menu_item)
-- Clipping and scrolling: a scroll area clips what is drawn inside it (and the regions: a widget scrolled out of
-- sight takes no mouse); things that must lie over everything (an open dropdown's list) are drawn after all draw
-- handlers (uis.defer).
local uis = {}            -- the scroll areas' and deferred drawing's state (one local: the main chunk has few left)
uis.clip_stack = {}    -- this tick's open clips {x, y, w, h}, innermost last
uis.deferred = {}      -- functions run after every mod's draw handlers, in order
uis.wheel = 0          -- wheel notches over mod UI this tick (+ away from the player, as WM_MOUSEWHEEL)
uis.wheel_taken = false
uis.pad_scroll = 0     -- D-pad lines in a scroll area this tick (+ down)
uis.scroll_now = {n = 0}    -- this tick's scroll areas {key, x, y, w, h, layer}; entries are reused (n = how many are live)
uis.scroll_prev = {n = 0}
uis.input_list = {n = 0}   -- the handles in ui_input
uis.input_pool = {}
uis.NO_INPUT = {}          -- read-only stand-in for a region without events
function uis.clear_list(t)
    for i = #t, 1, -1 do t[i] = nil end
end
uis.scroll_hot = nil   -- the key of the topmost scroll area under the cursor (last tick's)
uis.scroll_focus = nil -- the key of the scroll area clicked last (its keys work while the cursor is elsewhere)
uis.SCROLLBAR = 12     -- the scrollbar's width
-- Replaced windows (ui.replace_window): name -> the mod that draws its own screen in the window's place. Published
-- to Gear every tick before the handlers run; a mod that is off or unloaded does not count (the game's window shows).
uis.replace = {}
uis.typed = ''         -- the characters typed since the last tick (while a mod captures the keys)
-- Drag and drop between mods (ui.drag_source, ui.drop_target, ui.world_drop). One drag at a time: uis.drag = {type,
-- data, label, icon, mod, source, id, handle, carry, released, ...}. Drop targets register while a drag is on (each
-- tick, from the mods' draw handlers); the end of the tick decides what is under the cursor, shows the hint and drops.
uis.drag = nil
uis.drop_targets = {n = 0}      -- this tick's targets {handle, mod, opts, id}; entries are reused
uis.world_drops = {}            -- persistent world drop areas {mod, id, opts}, in registration order
uis.DRAG_THRESHOLD = 6          -- cursor travel (layout units) before a press on a source becomes a drag
uis.NOT_DRAGGING = setmetatable({dragging = false, over = false, valid = false}, {__newindex = function() error('read-only', 2) end})

-- Whether a target's `accept` (a type, a list of types, '*', or a function(drag) -> ok, text) takes the drag, and the
-- text for it (the hint when it does, the reason when it does not).
function uis.drag_accepts(spec, drag)
    local t = type(spec)
    if t == 'nil' then return true end
    if t == 'string' then return spec == '*' or spec == drag.type end
    if t == 'table' then
        for _, v in ipairs(spec) do
            if v == '*' or v == drag.type then return true end
        end
        return false
    end
    if t == 'function' then
        local ok, a, b = pcall(spec, drag)
        if not ok then
            if uis.drag_err ~= a then uis.drag_err = a; log('UI: a drop accept function failed: %s', tostring(a)) end
            return false
        end
        if type(a) == 'string' then return false, a end
        if a then return true, type(b) == 'string' and b or nil end
        return false, type(b) == 'string' and b or nil
    end
    return false
end

-- The topmost region of the last frame at (x, y), as ui_hot is decided for the cursor.
function uis.topmost_at(x, y)
    local best, found = -1, nil
    for i = 1, ui_prev_regions.n do
        local r = ui_prev_regions[i]
        local rank = r.layer * 100000 + i
        if rank > best and x >= r.x and y >= r.y and x < r.x + r.w and y < r.y + r.h then
            found, best = r.handle, rank
        end
    end
    return found
end

-- What the drag is over: -> target (an entry of uis.drop_targets or a world drop, nil: nothing that takes drops), valid
-- (true | false | nil: neutral, over its own source), text, spot (the world point of a world drop).
function uis.drag_resolve(drag, handle)
    if handle == drag.handle then return nil, nil end
    if handle then
        for i = 1, uis.drop_targets.n do
            local t = uis.drop_targets[i]
            if t.handle == handle then
                local ok, text = uis.drag_accepts(t.opts.accept, drag)
                if ok and not text then text = t.opts.hint end
                return t, ok and true or false, text
            end
        end
        return nil, false
    end
    -- over no mod region: the world, if the cursor is on it (not on a game window)
    if not native.world_cursor then return nil, false end
    local id, wx, wy, layer = native.world_cursor(0)
    if not id then return nil, false end
    local reason
    for _, w in ipairs(uis.world_drops) do
        if not (w.mod.disabled or w.mod.unloaded) then
            local ok, text = uis.drag_accepts(w.opts.accept, drag)
            if ok then
                return w, true, text or w.opts.hint, {x = wx, y = wy, layer = layer, object_id = id ~= 0 and id or nil}
            end
            reason = reason or text
        end
    end
    return nil, false, reason
end

function uis.drag_start(mod, id, handle, opts, carry)
    uis.drag = {
        type = opts.type, data = opts.data, label = opts.label, icon = opts.icon, icon_size = opts.icon_size,
        mod = mod, source = mod.id, id = id, handle = handle, carry = carry, on_end = opts.on_end,
        esc = native.key_down(0x1B) == true, x0 = ui_mouse.x, y0 = ui_mouse.y,
    }
    -- The keyboard stays from the game while a drag is on: Esc cancels it, and must not also open the game menu.
    key_capture[uis] = true
end

function uis.drag_end(dropped)
    local d = uis.drag
    uis.drag = nil
    key_capture[uis] = nil
    if d and d.on_end then call(d.mod, 'drag end', d.on_end, d, dropped and true or false) end
end
function uis.publish_replace()
    local names = {}
    for name, m in pairs(uis.replace) do
        if m.disabled or m.unloaded then uis.replace[name] = nil else names[#names + 1] = name end
    end
    if native.ui_replace_claims then native.ui_replace_claims(names) end
end

function uis.defer(fn)
    uis.deferred[#uis.deferred + 1] = fn
end

function uis.set_clip(c)
    if not native.ui_clip then return end
    if c then native.ui_clip(c.x, c.y, c.w, c.h) else native.ui_clip() end
end

-- Scroll math (also for the kernel tests): the offset clamped to 0..max, the thumb's size and place in a track.
function uis.scroll_clamp(offset, content, view)
    local max = math.max(0, content - view)
    return math.max(0, math.min(max, offset)), max
end
function uis.scroll_thumb(offset, content, view, track)
    local max = math.max(0, content - view)
    local size = math.max(16, math.min(track, track * view / math.max(content, 1)))
    local pos = max > 0 and (track - size) * offset / max or 0
    return size, pos
end
function uis.scroll_from_thumb(pos, content, view, track, size)
    local max = math.max(0, content - view)
    if track - size <= 0 then return 0 end
    return math.max(0, math.min(max, pos / (track - size) * max))
end
kernel.scroll_math = {clamp = uis.scroll_clamp, thumb = uis.scroll_thumb, from_thumb = uis.scroll_from_thumb}

uis.handles_by_mod = {}   -- mod id -> widget id (as given: a string or a number) -> handle; looked up every frame, so no string is built
local function ui_handle(mod, id)
    local per_mod = uis.handles_by_mod[mod.id]
    if not per_mod then
        per_mod = {}
        uis.handles_by_mod[mod.id] = per_mod
    end
    local h = per_mod[id]
    if not h then
        local key = mod.id .. '\0' .. tostring(id)
        h = ui_handles[key]
        if not h then
            h = ui_next_handle
            ui_next_handle = ui_next_handle + 1
            ui_handles[key] = h
            ui_handle_key[h] = key
        end
        per_mod[id] = h
    end
    return h
end

local function ui_begin_tick()
    for k in pairs(ui_counts) do ui_counts[k] = nil end
    ui_slots = nil
    ui_prev_regions, ui_regions = ui_regions, ui_prev_regions      -- two pooled arrays: entries are overwritten, `n` counts the live ones
    ui_regions.n = 0
    ui_panel_prev, ui_panel_list = ui_panel_list, ui_panel_prev
    for i = #ui_panel_list, 1, -1 do ui_panel_list[i] = nil end     -- two lists swapped each tick, the old one emptied
    ui_tooltip = nil
    ui_nav.n, ui_window_open = 0, false
    do
        local nudge, back, menu = native.ui_pad()
        ui_pad.nudge, ui_pad.back, ui_pad.menu = nudge or 0, back or 0, menu or 0
        ui_pad.page = (native.ui_page and native.ui_page() or 0)
        ui_pad.select = (native.ui_select and native.ui_select() or 0)
        -- B closes an open dropdown first.
        if ui_pad.back > 0 and ui_dropdown then
            ui_dropdown = nil
            ui_pad.back = ui_pad.back - 1
        end
    end
    uis.clear_list(uis.clip_stack)
    uis.clear_list(uis.deferred)
    uis.drop_targets.n = 0
    uis.set_clip(nil)
    do
        local wheel, pad = 0, 0
        if native.ui_wheel then wheel, pad = native.ui_wheel() end
        uis.wheel, uis.pad_scroll, uis.wheel_taken = (wheel or 0) / 120, pad or 0, false
    end
    uis.scroll_prev, uis.scroll_now = uis.scroll_now, uis.scroll_prev      -- two pooled lists (entries overwritten, `n` = live ones)
    uis.scroll_now.n = 0
    -- A document that was not drawn in the last frame (hidden, its mod stopped drawing it) gives up its focus and the keyboard capture.
    if uis.rml_docs then
        for _, entry in pairs(uis.rml_docs) do
            local d = entry.doc
            if d.drawn_frame ~= uis.frame and not d.idle_released and (d.drawn_frame or d.capturing) then d:release_focus() end
        end
    end
    uis.frame = (uis.frame or 0) + 1
    uis.modal, uis.modal_next = uis.modal_next, nil      -- the document that was modal in the last frame (draw order between mods is not fixed)
    ui_tick_state = native.ui_state()
    ui_mouse.x, ui_mouse.y = ui_tick_state.mouse_x or 0, ui_tick_state.mouse_y or 0
    -- The topmost scroll area under the cursor takes the wheel (layers in draw order, then the order drawn).
    uis.scroll_hot = nil
    do
        local best = -1
        for i = 1, uis.scroll_prev.n do
            local a = uis.scroll_prev[i]
            local rank = a.layer * 100000 + i
            if rank > best and ui_mouse.x >= a.x and ui_mouse.y >= a.y and ui_mouse.x < a.x + a.w and ui_mouse.y < a.y + a.h then
                uis.scroll_hot, best = a.key, rank
            end
        end
    end
    -- The topmost region of the last frame under the cursor (layers in draw order, then the order registered).
    ui_hot = nil
    local best = -1
    for i = 1, ui_prev_regions.n do
        local r = ui_prev_regions[i]
        local rank = r.layer * 100000 + i
        if rank > best and ui_mouse.x >= r.x and ui_mouse.y >= r.y and ui_mouse.x < r.x + r.w and ui_mouse.y < r.y + r.h then
            ui_hot, best = r.handle, rank
        end
    end
    local now = native.time()
    for h in pairs(ui_hover_since) do
        if h ~= ui_hot then ui_hover_since[h] = nil end
    end
    if ui_hot and not ui_hover_since[ui_hot] then ui_hover_since[ui_hot] = now end
    for i = uis.input_list.n, 1, -1 do        -- last tick's per-region states go back to the pool
        local h = uis.input_list[i]
        local st = ui_input[h]
        st.pressed, st.released, st.clicked = nil, nil, nil
        ui_input[h] = nil
        uis.input_pool[#uis.input_pool + 1] = st
        uis.input_list[i] = nil
    end
    uis.input_list.n = 0
    for _, e in ipairs(native.ui_events()) do
        local s = ui_input[e.region]
        if not s then
            local np = #uis.input_pool
            if np > 0 then s = uis.input_pool[np]; uis.input_pool[np] = nil else s = {} end
            ui_input[e.region] = s
            local n = uis.input_list.n + 1
            uis.input_list.n = n
            uis.input_list[n] = e.region
        end
        -- The button's own position: the cursor read at the tick may still be the one before it moved there.
        ui_mouse.x, ui_mouse.y = e.x, e.y
        if e.down then
            if e.button == 0 then
                s.pressed = true
                ui_active = e.region
            end
        else
            s.released = true
            local dr = uis.drag
            local dragged = dr and not dr.carry and dr.handle == e.region     -- a drag's release is no click on its source
            if dragged then dr.released, dr.rx, dr.ry = true, e.x, e.y end
            if ui_active == e.region then
                ui_active = nil
                -- A click: released over the region it was pressed on.
                for ri = 1, dragged and 0 or ui_prev_regions.n do
                    local r = ui_prev_regions[ri]
                    if r.handle == e.region and e.x >= r.x and e.y >= r.y and e.x < r.x + r.w and e.y < r.y + r.h then
                        s.clicked = true
                    end
                end
            end
        end
    end
end

-- Fonts. nil or 'ui': the vector UI font (drawn for the screen's pixels; opts.size in layout units, opts.face, bold,
-- italic, shadow, outline); a number or 'game1'..'game7': the game's own bitmap font of that id (scaled with the UI).
local UI_TEXT_SIZE = 13
local function ui_font(opts, level)
    local f = opts.font
    if f == nil or f == 'ui' then return -1 end
    if type(f) == 'number' then return f end
    local n = type(f) == 'string' and f:match('^game(%d+)$')
    if n then return tonumber(n) end
    error('font is "ui", "game1".."game7" or a game font id', (level or 2) + 1)
end
local function ui_font_style(opts)
    return bit.bor(opts.bold and 1 or 0, opts.italic and 2 or 0, opts.shadow and 4 or 0, opts.outline and 8 or 0)
end
-- Width and height of text in layout units, or nil.
local function ui_measure(text, opts)
    local font = ui_font(opts, 2)
    if font == -1 then
        return native.ui_text_size(text, -1, opts.face, opts.size or UI_TEXT_SIZE, ui_font_style(opts))
    end
    return native.ui_text_size(text, font)
end

-- The tooltip of the widget hovered long enough, drawn last on the top layer beside the cursor.
local UI_TOOLTIP_DELAY = 0.4
-- The game's cursor sprite (an arrow whose tip is the hotspot) reaches this far right and down of the hotspot, in layout
-- units (measured at the 1024 x 768 layout; the sprite is smaller in layout units at higher resolutions).
local UI_CURSOR_W, UI_CURSOR_H = 30, 38

-- Where a w x h tooltip goes for a cursor at (mx, my): the first of below-right, below-left, above-right, above-left,
-- right of and left of the cursor that lies inside `bounds` ({left, top, right, bottom}) and clear of the cursor
-- sprite (cw x ch from the hotspot); if none does, the first clamped into the bounds and pushed off the sprite.
local function tooltip_place(w, h, mx, my, bounds, cw, ch, gap)
    local l, t, r, b = bounds[1], bounds[2], bounds[3], bounds[4]
    local function inside(x, y) return x >= l and y >= t and x + w <= r and y + h <= b end
    -- candidates: below / above the sprite, left or right aligned to the hotspot; beside it, level with the hotspot
    local cands = {
        {mx + gap, my + ch + gap},
        {mx - w - gap, my + ch + gap},
        {mx + gap, my - h - gap},
        {mx - w - gap, my - h - gap},
        {mx + cw + gap, my},
        {mx - w - gap, my},
    }
    for _, c in ipairs(cands) do
        if inside(c[1], c[2]) then return c[1], c[2] end
    end
    -- last resort: clamp the preferred place, then move it off the sprite (up, else right, else left)
    local x = math.max(l, math.min(cands[1][1], r - w))
    local y = math.max(t, math.min(cands[1][2], b - h))
    if x < mx + cw and x + w > mx and y < my + ch and y + h > my then
        if my - h - gap >= t then y = my - h - gap
        elseif mx + cw + gap + w <= r then x = mx + cw + gap
        else x = math.max(l, mx - w - gap) end
    end
    return x, y
end
-- Tells SacredBild which widgets the pad can point at. An open dropdown's list, or the screen of a mod that asked
-- (ui.nav_modal), is all there is; else every widget, only those of the top layer while the game's menu or a
-- message box is up.
local function ui_push_nav()
    local s = ui_tick_state or {}
    local out = uis.nav_out
    if not out then out = {}; uis.nav_out = out end
    local count = ui_nav.n
    -- Which stops: an open dropdown's list, or the modal screen's, or all (the top layer's only while the game's menu is up).
    local mode, flags = 'all', 0
    for i = 1, count do
        if ui_nav[i].drop then mode, flags = 'drop', 1; break end
    end
    if mode == 'all' then
        if uis.modal then mode, flags = 'modal_doc', 1
        elseif ui_nav_modal then mode, flags = 'modal_mod', 1
        else
            if s.escape_menu or s.message_box then mode = 'top' end
            if ui_window_open then flags = 2 end
        end
    end
    local n = 0
    local nn = 0
    local home_row
    local nat = uis.nav_native
    if not nat then nat = {}; uis.nav_native = nat end
    for i = 1, count do
        local e = ui_nav[i]
        local take
        if mode == 'drop' then take = e.drop
        elseif mode == 'modal_mod' then take = e.mod == ui_nav_modal
        elseif mode == 'top' then take = e.layer == 2
        else take = true end
        if take and n < 400 then
            n = n + 1
            local row = out[n]
            if not row then row = {}; out[n] = row end
            row.x, row.y = e.ax or (e.x + e.w / 2), e.ay or (e.y + e.h / 2)
            row.left, row.top, row.width, row.height, row.kind = e.x, e.y, e.w, e.h, e.kind or 0
            if e.home and not home_row then home_row = n end
            if e.native and nn < 400 then
                -- a control that draws its own button prompt (an RmlUi element): SacredBild keeps its own off it
                nn = nn + 1
                local nrow = nat[nn]
                if not nrow then nrow = {}; nat[nn] = nrow end
                nrow.left, nrow.top, nrow.width, nrow.height = e.x, e.y, e.w, e.h
            end
        end
    end
    for i = n + 1, #out do out[i] = nil end        -- the host counts the rows by the table's length
    if home_row and home_row > 1 then out[1], out[home_row] = out[home_row], out[1] end     -- the first stop is where the cursor starts on a screen that opens
    for i = nn + 1, #nat do nat[i] = nil end
    native.ui_nav(out, flags)
    if native.ui_nav_native then native.ui_nav_native(nat) end
end

-- Widest a tooltip's text gets before it wraps, in layout units.
uis.TOOLTIP_MAX_W = 360

-- text broken at spaces (and at \n) into lines no wider than `width`; a word wider than that keeps a line of its own.
-- Returns the lines and the width of the widest. (A field of uis: the kernel is at Lua's limit of 200 locals.)
function uis.tooltip_wrap(text, width)
    local lines, widest = {}, 0
    for para in (text .. '\n'):gmatch('(.-)\n') do
        local line, lw = nil, 0
        for word in para:gmatch('%S+') do
            local try = line and (line .. ' ' .. word) or word
            local tw = ui_measure(try, {}) or (#try * 6)
            if tw <= width or not line then
                line, lw = try, tw
            else
                lines[#lines + 1] = line
                widest = math.max(widest, lw)
                line = word
                lw = ui_measure(word, {}) or (#word * 6)
            end
        end
        lines[#lines + 1] = line or ''
        widest = math.max(widest, lw)
    end
    return lines, widest
end

-- The drag's hint at the cursor: the dragged thing (its icon or label), a badge (+ it takes the drop here, x it does not;
-- none while over its own source) and the target's text (the hint, or why it does not take it).
function uis.drag_draw(drag, valid, text)
    local s = ui_tick_state or {}
    local bl, bt = s.screen_left or 0, s.screen_top or 0
    local br, bb = s.screen_right or 1024, s.screen_bottom or 768
    local gw, gh
    local lw, lh = 0, 0
    if drag.icon then
        gw = drag.icon_size or 32
        gh = gw
    elseif drag.label then
        lw, lh = ui_measure(tostring(drag.label), {})
        gw, gh = math.ceil(lw or 0) + 12, math.max(lh or 20, 14) + 4
    else
        gw, gh = 0, 0
    end
    local tw, th = 0, 0
    if text and text ~= '' then
        tw, th = ui_measure(tostring(text), {})
        tw, th = math.ceil(tw or 0) + 12, math.max(th or 20, 14) + 4
    end
    local badge = valid ~= nil and 16 or 0
    local total = gw + (badge > 0 and badge - 6 or 0) + (tw > 0 and tw + 4 or 0)
    local x, y = ui_mouse.x + 16, ui_mouse.y + 18
    if x + total > br - 4 then x = math.max(bl, ui_mouse.x - 6 - total) end
    if y + math.max(gh, th, badge) > bb - 4 then y = math.max(bt, bb - 4 - math.max(gh, th, badge)) end
    if drag.icon then
        native.ui_image(2, -1, drag.icon, x, y, gw, gh, 0xC8FFFFFF, 1)
    elseif drag.label then
        native.ui_rect(2, -1, x, y, gw, gh, 0xD0101018)
        native.ui_rect(2, -1, x, y, gw, 1, 0xFFC8AA5A)
        native.ui_rect(2, -1, x, y + gh - 1, gw, 1, 0xFFC8AA5A)
        native.ui_rect(2, -1, x, y, 1, gh, 0xFFC8AA5A)
        native.ui_rect(2, -1, x + gw - 1, y, 1, gh, 0xFFC8AA5A)
        native.ui_text(2, -1, tostring(drag.label):gsub('\\', '/'), x + 6, y + 2, 0, gh - 4, 0xFFF0F0F0, -1, 0, 0, 0, nil,
            UI_TEXT_SIZE, 0, 1)
    end
    local bx = x + math.max(0, gw - 6)
    if badge > 0 then
        local color = valid and 0xFF2E9E4F or 0xFFC83232
        native.ui_rect(2, -1, bx, y, badge, badge, 0xFFFFFFFF)
        native.ui_rect(2, -1, bx + 1, y + 1, badge - 2, badge - 2, color)
        native.ui_text(2, -1, valid and '+' or '\195\151', bx, y, badge, badge, 0xFFFFFFFF, -1, 1, 0, 0, nil, 14, 0, 1)
    end
    if tw > 0 then
        local tx = bx + badge + 4
        local ty = y
        native.ui_rect(2, -1, tx, ty, tw, th, 0xE0101018)
        native.ui_rect(2, -1, tx, ty, tw, 1, 0xFFC8AA5A)
        native.ui_rect(2, -1, tx, ty + th - 1, tw, 1, 0xFFC8AA5A)
        native.ui_rect(2, -1, tx, ty, 1, th, 0xFFC8AA5A)
        native.ui_rect(2, -1, tx + tw - 1, ty, 1, th, 0xFFC8AA5A)
        native.ui_text(2, -1, tostring(text):gsub('\\', '/'), tx + 6, ty + 2, 0, th - 4, 0xFFF0F0F0, -1, 0, 0, 0, nil, UI_TEXT_SIZE, 0, 1)
    end
end

-- End of every tick while a drag is on: cancel (Esc, the pad's B, the source's mod gone, a lost release), find the
-- target under the cursor, drop on a release (or on a click, when the drag is carried with the pad), else show the hint.
function uis.drag_finish()
    local d = uis.drag
    if not d then return end
    ui_tooltip = nil
    local cancel = d.mod.disabled or d.mod.unloaded
    if native.key_down(0x1B) then
        if not d.esc then cancel = true end
        d.esc = true
    else
        d.esc = false
    end
    if ui_pad.back > 0 then
        ui_pad.back = ui_pad.back - 1
        cancel = true
    end
    if not cancel and not d.carry and not d.released and ui_active ~= d.handle then cancel = true end
    if cancel then
        uis.drag_end(false)
        return
    end
    local handle = ui_hot
    if d.released then handle = uis.topmost_at(d.rx, d.ry) end
    local target, valid, text, spot = uis.drag_resolve(d, handle)
    local dropped = false
    if d.carry then
        local st = target and not target.world and ui_input[target.handle]
        dropped = valid and st and st.clicked or false
    else
        dropped = d.released and valid and target or false
    end
    if dropped then
        local info = spot or {x = d.released and d.rx or ui_mouse.x, y = d.released and d.ry or ui_mouse.y}
        uis.drag_end(true)
        call(target.mod, 'drop', target.opts.on_drop, d, info)
        return
    end
    if d.released then
        uis.drag_end(false)
        return
    end
    uis.drag_draw(d, valid, text)
end

local function ui_end_tick()
    -- What must lie over everything: open dropdown lists, drawn after all draw handlers, outside any clip.
    uis.clear_list(uis.clip_stack)
    uis.set_clip(nil)
    -- Run by index: a deferred function may defer another; the list is emptied once all have run.
    local deferred = uis.deferred
    local i = 1
    while deferred[i] do
        local ok, err = pcall(deferred[i])
        if not ok then log('UI: a deferred draw failed: %s', tostring(err)) end
        uis.clear_list(uis.clip_stack)
        uis.set_clip(nil)
        i = i + 1
    end
    uis.clear_list(deferred)
    uis.drag_finish()
    ui_push_nav()
    local t = ui_tooltip
    if not t then return end
    local s = ui_tick_state or {}
    local bl, bt = s.screen_left or 0, s.screen_top or 0
    local br, bb = s.screen_right or 1024, s.screen_bottom or 768
    -- wrap to at most uis.TOOLTIP_MAX_W units of text, and never wider than the screen less a margin
    local max_text = math.max(60, math.min(uis.TOOLTIP_MAX_W, (br - bl) - 24 - 12))
    local lines, tw = uis.tooltip_wrap(tostring(t.text), max_text)
    local _, line_h = ui_measure('Ag', {})
    line_h = math.max(line_h or 20, 14)
    local w = math.ceil(tw) + 12
    local h = #lines * line_h + math.max(0, 20 - line_h)
    local x, y = tooltip_place(w, h, ui_mouse.x, ui_mouse.y, {bl, bt, br, bb}, UI_CURSOR_W, UI_CURSOR_H, 4)
    native.ui_rect(2, -1, x, y, w, h, 0xE0101018)
    native.ui_rect(2, -1, x, y, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y + h - 1, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y, 1, h, 0xFFC8AA5A)
    native.ui_rect(2, -1, x + w - 1, y, 1, h, 0xFFC8AA5A)
    local ty = y + math.floor(math.max(0, 20 - line_h) / 2)
    for i, line in ipairs(lines) do
        if line ~= '' then
            native.ui_text(2, -1, line, x + 6, ty + (i - 1) * line_h, 0, line_h, 0xFFF0F0F0, -1, 0, 0, 0, nil,
                UI_TEXT_SIZE, 0, 1)
        end
    end
end

-- The layout (mods\gear_layout.json): where the player put the mods' panels ("mod_id:panel_id") and the game's
-- windows ("game:UI_WND_INVENTORY"). Stored in a virtual 4096 x 4096 screen (the whole screen, whatever its
-- resolution and aspect), so a layout keeps its places when the resolution changes.
local layout = nil          -- key -> {x, y, w, h} in virtual units
local layout_dirty = false
local ui_edit = false       -- the layout edit mode: panels show their ids and resize grips

local function layout_get()
    if not layout then
        local text = native.layout_read and native.layout_read()
        local v = text and json.decode(text)
        layout = type(v) == 'table' and v or {}
    end
    return layout
end

-- A panel's open / closed state (`open`) lives in its entry too and outlives moves, resizes and resets of the place.
local function layout_set(key, entry)
    local lay = layout_get()
    local old = lay[key]
    if old and old.open ~= nil then
        entry = entry or {}
        entry.open = old.open
    end
    lay[key] = entry
    layout_dirty = true
end

local function screen_rect()
    local s = ui_tick_state or {}
    local l, t = s.screen_left or 0, s.screen_top or 0
    local r, b = s.screen_right or 1024, s.screen_bottom or 768
    if r - l < 1 then r = l + 1024 end
    if b - t < 1 then b = t + 768 end
    return l, t, r, b
end

local function to_virtual(x, y)
    local l, t, r, b = screen_rect()
    return (x - l) / (r - l) * 4096, (y - t) / (b - t) * 4096
end

local function from_virtual(vx, vy)
    local l, t, r, b = screen_rect()
    return l + vx / 4096 * (r - l), t + vy / 4096 * (b - t)
end

local function size_to_virtual(w, h)
    local l, t, r, b = screen_rect()
    return w / (r - l) * 4096, h / (b - t) * 4096
end

local function size_from_virtual(vw, vh)
    local l, t, r, b = screen_rect()
    return vw / 4096 * (r - l), vh / 4096 * (b - t)
end

-- The game's windows the layout moved: placed every tick (cheap), so they follow resolution changes and new games.
local function apply_game_layout()
    local lay = layout_get()
    if not next(lay) or not native.ui_can_move_windows() then return end
    for _, w in ipairs(native.ui_windows()) do
        local e = w.movable and lay['game:' .. w.name]
        if e then
            local sx, sy = from_virtual(e.x, e.y)
            native.ui_move_window(w.slot, sx - w.x, sy - w.y, true)
        end
    end
end

local function save_layout()
    if layout_dirty and not ui_active then
        layout_dirty = false
        native.layout_write(json.encode(layout_get()))
    end
end

-- Key names for gear.ui.key_pressed.
local UI_VK = {
    escape = 0x1B, enter = 0x0D, space = 0x20, tab = 0x09, shift = 0x10, control = 0x11, alt = 0x12,
    left = 0x25, up = 0x26, right = 0x27, down = 0x28, insert = 0x2D, delete = 0x2E, home = 0x24, ['end'] = 0x23,
    pageup = 0x21, pagedown = 0x22,
    ctrl = 0x11, backspace = 0x08, pause = 0x13, capslock = 0x14,
    semicolon = 0xBA, equals = 0xBB, comma = 0xBC, minus = 0xBD, period = 0xBE, slash = 0xBF, backquote = 0xC0,
    lbracket = 0xDB, backslash = 0xDC, rbracket = 0xDD, quote = 0xDE,
    multiply = 0x6A, add = 0x6B, subtract = 0x6D, decimal = 0x6E, divide = 0x6F,
}
for i = 0, 9 do UI_VK['num' .. i] = 0x60 + i end
for i = 1, 24 do UI_VK['f' .. i] = 0x6F + i end
-- Mouse buttons 3 to 5 (middle, and the two side buttons), as polled keys: bindable like any key. Mice with more
-- buttons (MMO mice) send those as keys, usually F13 to F24 or digits, which the names above cover.
UI_VK.mouse3, UI_VK.mouse4, UI_VK.mouse5 = 0x04, 0x05, 0x06
for c = string.byte('a'), string.byte('z') do UI_VK[string.char(c)] = c - 32 end
for c = string.byte('0'), string.byte('9') do UI_VK[string.char(c)] = c end

local function make_ui(mod)
    local ui = {WIDTH = 1024, HEIGHT = 768}
    function ui.rgba(r, g, b, a)
        return ui_color({r, g, b, a == nil and 255 or a})
    end
    -- ui.rect(x, y, w, h, color, {layer, window})
    function ui.rect(x, y, w, h, color, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        ui_count(mod)
        return native.ui_rect(layer, slot, x, y, w, h, ui_color(color, 0xC0000000))
    end
    -- ui.frame(x, y, w, h, color, thickness, opts): an outline, `thickness` (default 1) inside the box.
    function ui.frame(x, y, w, h, color, thickness, opts)
        local t = thickness or 1
        ui.rect(x, y, w, t, color, opts)
        ui.rect(x, y + h - t, w, t, color, opts)
        ui.rect(x, y + t, t, h - 2 * t, color, opts)
        return ui.rect(x + w - t, y + t, t, h - 2 * t, color, opts)
    end
    -- ui.text(text, x, y, {color, font, size, face, bold, italic, shadow, outline, width, height, align, background,
    -- padding, markup, flags, layer, window})
    function ui.text(text, x, y, opts)
        opts = opts or {}
        text = tostring(text)
        local font = ui_font(opts)
        -- Backslashes start the game fonts' markup (\c colors, \g images ...); plain text keeps them out.
        if font ~= -1 and not opts.markup then text = text:gsub('\\', '/') end
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local align = UI_ALIGN[opts.align or 'left']
        if not align then error('align is "left", "center" or "right"', 2) end
        local valign = UI_VALIGN[opts.valign or 'top']
        if not valign then error('valign is "top", "middle", "bottom" or "baseline"', 2) end
        ui_count(mod)
        return native.ui_text(layer, slot, text, x, y, opts.width or 0, opts.height or 0,
            ui_color(opts.color, 0xFFFFFFFF), font, bit.bor(align, opts.flags or 0) % 4294967296,
            ui_color(opts.background, 0), opts.padding or 0, opts.face, opts.size or UI_TEXT_SIZE, ui_font_style(opts),
            valign)
    end
    -- ui.image(id, x, y, w, h, {fit = 'stretch' | 'contain' | 'cover', flip = 'h' | 'v' | 'hv', color, layer, window}); w, h default to the
    -- image's own size.
    function ui.image(id, x, y, w, h, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local fit = UI_FITS[opts.fit or 'stretch']
        if not fit then error('fit is "stretch", "contain" or "cover"', 2) end
        if opts.flip then
            local flip = UI_FITS[1][opts.flip]
            if not flip then error('flip is "h", "v" or "hv"', 2) end
            fit = fit + flip
        end
        ui_count(mod)
        local cr = opts.crop
        if cr then
            return native.ui_image(layer, slot, id, x, y, w or 0, h or 0, ui_color(opts.color, 0xFFFFFFFF), fit,
                cr[1] or 0, cr[2] or 0, cr[3] or 1, cr[4] or 1)
        end
        return native.ui_image(layer, slot, id, x, y, w or 0, h or 0, ui_color(opts.color, 0xFFFFFFFF), fit)
    end
    -- ui.item_icon(item_or_type, x, y, w, h, {layer, window}): what the inventory shows for an item (its own 3D model),
    -- as large as fits the box, centered in it. `item_or_type`: an item table (gear.world.item, gear.inv container
    -- items: its object is drawn), a type id or a type name (an item of that type is made for the frame).
    function ui.item_icon(item, x, y, w, h, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local id, kind = 0, 0
        if type(item) == 'table' then
            id, kind = item.id or 0, item.type or 0
        elseif type(item) == 'number' then
            kind = item
        elseif type(item) == 'string' then
            kind = data.object_types:find(item) or error('no item type named ' .. item, 2)
        else
            error('item_icon wants an item, a type id or a type name', 2)
        end
        assert(type(x) == 'number' and type(y) == 'number' and type(w) == 'number' and type(h) == 'number',
            'x, y, w and h must be numbers')
        ui_count(mod)
        return native.ui_item_icon(layer, slot, x, y, w, h, id, kind)
    end
    -- ui.item_tooltip(item, x, y) -> true | nil, message: the game's own tooltip popup for an item (framed, name in the
    -- rarity colour, price, protection, level, bonuses) with its top-left corner at (x, y) of the 1024x768 layout, kept on the
    -- screen. `item`: an item table (gear.world.item, container:items()), an item id, or an export string
    -- (gear.inv.export: a temporary item is made to read the text from). Call it every frame the tooltip should show.
    function ui.item_tooltip(item, x, y)
        assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
        if type(item) == 'table' then item = item.id end
        if type(item) ~= 'number' and type(item) ~= 'string' then error('item_tooltip wants an item, an id or an export', 2) end
        return native.item_tooltip(item, x, y)
    end
    -- ui.item_tooltip_text(item) -> text | nil, message: the words of the game's item tooltip (UTF-8, with the game's
    -- colour codes) instead of a popup; same arguments as item_tooltip.
    function ui.item_tooltip_text(item)
        if type(item) == 'table' then item = item.id end
        if type(item) ~= 'number' and type(item) ~= 'string' then error('item_tooltip_text wants an item, an id or an export', 2) end
        return native.item_tooltip_text(item)
    end
    -- ui.hovered_item() -> item, x, y, w, h | nil: the item whose tooltip the game shows right now (cursor or pad cursor resting on
    -- an item in the backpack, chest, shop, equipment ...) as a gear.world.item table, and the tooltip's VISIBLE outer rectangle (the
    -- frame's bars, not the corner ornaments) in layout units. The popup's own numbers are the game canvas's (its origin is the
    -- screen's left edge, the layout's is the 1024x768 box's: they differ by screen_left in a wider window; a window's popups are
    -- in the window's frame, which SacredBild reports: all converted here) and its size is
    -- that of the text area, 3 short of the bars on the left and top, 6 and 4 on the right and bottom.
    function ui.hovered_item()
        local id, x, y, w, h, _, canvas = native.item_hover()
        if not id then return nil end
        local item = world.item(id)
        if not item then return nil end
        if canvas then
            local sl, st = ui.screen()
            x, y = x + sl, y + st
        end
        return item, x - 3, y - 3, w + 9, h + 7
    end
    -- ui.tooltip_anchor(w, h, {gap, margin, side}) -> x, y, side | nil: where a panel of the bars' outer size w x h (see
    -- ui.native.frame) goes to sit flush against the right side of the game's item tooltip, the corner ornaments of both touching
    -- (`gap` more space, default 0); on the left side when it does not fit on the right (`side` 'right' or 'left' forces one);
    -- kept `margin` (default 2) inside the screen and its ornaments' overhang.
    function ui.tooltip_anchor(w, h, opts)
        assert(type(w) == 'number' and type(h) == 'number', 'w and h must be numbers')
        opts = opts or {}
        local _, rx, ry, rw, rh = ui.hovered_item()
        if not rx then return nil end
        local inset, over = ui.native.insets()
        local gap = over.right + over.left + (opts.gap or 0)
        local margin = opts.margin or 2
        local sl, st, sr, sb = ui.screen()
        local side = opts.side
        if side ~= 'left' and side ~= 'right' then
            side = (rx + rw + gap + w + over.right > sr - margin) and 'left' or 'right'
        end
        local x = side == 'right' and rx + rw + gap or rx - gap - w
        x = math.max(sl + margin + over.left, math.min(x, sr - margin - over.right - w))
        local y = math.max(st + margin + over.top, math.min(ry, sb - margin - over.bottom - h))
        return x, y, side
    end
    function ui.image_info(id)
        local info = type(id) == 'number' and native.ui_image_info(id)
        if info then info.id = id end
        return info or nil
    end
    -- ui.images(filter): the UI images whose sheet name contains `filter` (any case), {id, sheet, width, height}.
    function ui.images(filter)
        local out, f = {}, filter and filter:lower()
        for id = 1, native.ui_image_count() - 1 do
            local info = native.ui_image_info(id)
            if info and (not f or info.sheet:lower():find(f, 1, true)) then
                info.id = id
                out[#out + 1] = info
            end
        end
        return out
    end
    -- ui.texture_image(name): the id of a texture of the game's Texture.pak (by file name, any case) for ui.image; nil if there is none.
    function ui.texture_image(name)
        assert(type(name) == 'string', 'name must be a string')
        return native.texture_image(name)
    end
    function ui.fonts() return native.ui_font_count() end

    ---------------------------------------------------------------------------------------------------------
    -- gear.ui.native: pieces of the game's own look, built from its UI images (ui.images). Each piece is drawn with
    -- ui.image / ui.rect, so layers, windows, the 2000 commands per tick and the 1024x768 layout apply as there.
    -- A style is a table of image ids and sizes; `frame` draws one. Add styles (ring, bar ...) next to `tooltip`.
    ---------------------------------------------------------------------------------------------------------
    local native_ui = {}
    ui.native = native_ui
    -- tooltip: the frame of the game's item tooltip (TL = 54, TR = 55, the bottom corners are those flipped upside down;
    -- bar 52 at the sides, 14 / 21 on top and bottom; the fill is the game's 0x9F000000).
    local NATIVE_FRAMES = {
        tooltip = {
            fill = 0x9F000000,
            tl = {id = 54, w = 37, h = 19}, tr = {id = 55, w = 37, h = 17},
            left = {id = 52, w = 6}, right = {id = 52, w = 7},
            top = {id = 14, h = 6}, bottom = {id = 14, h = 6, flip = 'v'},
            overhang = {left = 11, top = 5, right = 11, bottom = 5},     -- the corner ornaments reach beyond the bars
            inset = {left = 6, top = 6, right = 6, bottom = 6},
        },
        -- thick: the frame of the character selection and the other big menu screens (pieces of the menus' UI images, positions measured against the
        -- game's own frame by template matching): the rounded corner pieces 318 / 319 (41x44, 39x44) and 320 / 321 (41x42, 39x42) at the corners of the box,
        -- pattern strips 294 (32x17, 8.5 from the top edge; the bottom one flipped) between them, 310 / 312 (41x32, 24x32) down the sides, the crests 302 + 303
        -- (top) and 304 + 305 (bottom) in the middle of the top and bottom edge, dividers made of 294. The fill stays inside the strips.
        thick = {
            fill = 0x9F000000,
            kind = 'thick',
            inset = {left = 22, top = 24.5, right = 22, bottom = 24.5},
            overhang = {left = 0, top = 0, right = 0, bottom = 0},
        },
    }
    -- ui.native.insets(style) -> {left, top, right, bottom}: how far the frame's border reaches into the box (the text goes
    -- inside it) and `overhang`, how far the corner ornaments reach out of it.
    function native_ui.insets(style)
        local st = NATIVE_FRAMES[style or 'tooltip']
        if not st then error('unknown frame style', 2) end
        return copy(st.inset), copy(st.overhang)
    end
    -- ui.native.frame(x, y, w, h, {style = 'tooltip', layer, window, fill = color | false, color = tint}) -> drawn
    function native_ui.frame(x, y, w, h, opts)
        opts = opts or {}
        local st = NATIVE_FRAMES[opts.style or 'tooltip']
        if not st then error('unknown frame style (known: tooltip, thick)', 2) end
        local o = {layer = opts.layer, window = opts.window, color = opts.color}
        local function img(p, ix, iy, iw, ih)
            if iw > 0 and ih > 0 then
                ui.image(p.id, ix, iy, iw, ih, {layer = o.layer, window = o.window, color = o.color, flip = p.flip})
            end
        end
        if st.kind == 'thick' then return native_ui.thick_frame(x, y, w, h, opts, st) end     -- its fill is inset, see there
        if opts.fill ~= false then
            ui.rect(x, y, w, h, opts.fill or st.fill, {layer = o.layer, window = o.window})
        end
        local oh = st.overhang
        local lw, rw, th, bh = st.left.w, st.right.w, st.top.h, st.bottom.h
        img(st.top, x + lw, y, w - lw - rw, th)
        img(st.bottom, x + lw, y + h - bh, w - lw - rw, bh)
        img(st.left, x, y + th, lw, h - th - bh)
        img(st.right, x + w - rw, y + th, rw, h - th - bh)
        img({id = st.tl.id}, x - oh.left, y - oh.top, st.tl.w, st.tl.h)
        img({id = st.tr.id}, x + w + oh.right - st.tr.w, y - oh.top, st.tr.w, st.tr.h)
        img({id = st.tl.id, flip = 'v'}, x - oh.left, y + h + oh.bottom - st.tl.h, st.tl.w, st.tl.h)
        img({id = st.tr.id, flip = 'v'}, x + w + oh.right - st.tr.w, y + h + oh.bottom - st.tr.h, st.tr.w, st.tr.h)
        return true
    end

    -- The thick frame: segments of a pattern strip repeated along the sides (the last one cropped, so the pattern keeps its proportions), dividers at
    -- opts.dividers (offsets from the top of the box to the top of the divider's strip), the corner and crest pieces over the strips. The box is the
    -- outer rectangle of the corner pieces; the fill covers only what the strips enclose, so the rounded corners show the background behind their arcs.
    function native_ui.thick_frame(x, y, w, h, opts, st)
        local o = {layer = opts.layer, window = opts.window, color = opts.color}
        local function img(id, ix, iy, iw, ih, flip, crop)
            ui.image(id, ix, iy, iw, ih, {layer = o.layer, window = o.window, color = o.color, flip = flip, crop = crop})
        end
        local function tile_h(id, tw, th, tx, ty, len, flip)
            local at = 0
            while at < len - 0.01 do
                local seg = math.min(tw, len - at)
                img(id, tx + at, ty, seg, th, flip, seg < tw and {0, 0, seg / tw, 1} or nil)
                at = at + tw
            end
        end
        local function tile_v(id, tw, th, tx, ty, len)
            local at = 0
            while at < len - 0.01 do
                local seg = math.min(th, len - at)
                img(id, tx, ty + at, tw, seg, nil, seg < th and {0, 0, 1, seg / th} or nil)
                at = at + th
            end
        end
        -- Where the fill starts, from the box edge: the game sets it per window (the character selection and settings 22 / 24.5 / 22 / 24.5, where it touches the thin brown line inside the strips on all four sides, the popup dialog about
        -- 16 / 20.5 / 15 / 17.5, the in-game menu measured 20.5 / 22 / 20.5 (bottom not measured)); opts.fill_inset is one number for all sides or {left, top, right, bottom}.
        local ins = st.inset
        local fi = opts.fill_inset
        if type(fi) == 'number' then
            ins = {left = fi, top = fi, right = fi, bottom = fi}
        elseif type(fi) == 'table' then
            ins = {left = fi.left or fi[1] or ins.left, top = fi.top or fi[2] or ins.top, right = fi.right or fi[3] or ins.right, bottom = fi.bottom or fi[4] or ins.bottom}
        end
        if opts.fill ~= false then
            ui.rect(x + ins.left, y + ins.top, w - ins.left - ins.right, h - ins.top - ins.bottom, opts.fill or st.fill, {layer = o.layer, window = o.window})
        end
        -- The strips run on 8 units under the corner pieces (they are drawn last): the corner sprites have transparent margins and a hollow, and strips that stop at their edge leave slits.
        local under = 8
        tile_h(294, 32, 17, x + 41 - under, y + 8.5, w - 41 - 39 + 2 * under)
        tile_h(294, 32, 17, x + 41 - under, y + h - 25.5, w - 41 - 39 + 2 * under, 'v')
        tile_v(310, 41, 32, x, y + 44.8 - under, h - 44.8 - 42 + 2 * under)
        tile_v(312, 24, 32, x + w - 24, y + 44.8 - under, h - 44.8 - 42 + 2 * under)
        for _, d in ipairs(opts.dividers or {}) do tile_h(294, 32, 17, x + 21, y + d, w - 42) end
        local cx = x + w / 2
        img(302, cx - 32, y + 0.8, 32, 44); img(303, cx, y + 0.8, 32, 44)
        img(304, cx - 32, y + h - 42, 32, 42); img(305, cx, y + h - 42, 32, 42)
        img(318, x, y, 41, 44); img(319, x + w - 39, y, 39, 44)
        img(320, x, y + h - 42, 41, 42); img(321, x + w - 39, y + h - 42, 39, 42)
        return true
    end

    ---------------------------------------------------------------------------------------------------------
    -- Widgets (immediate mode): call them every frame with the current values; they draw themselves, take the
    -- mouse over their rectangles and return what the player did since the last frame. `id` names a widget
    -- within the mod (any string or number, unique per widget). opts.tooltip shows a text after the cursor rests
    -- on the widget; opts.layer as for drawing (default 'hud'); opts.enabled = false greys it out.
    ---------------------------------------------------------------------------------------------------------
    local C = {
        bg = 0xC8141420, bg_hover = 0xD8282838, bg_down = 0xE03C3C50, edge = 0xFFC8AA5A, edge_dim = 0xFF6E5E3A,
        text = 0xFFF0F0F0, text_dim = 0xFF8C8C8C, accent = 0xFFFFD780, title = 0xE0302418,
    }
    ui.COLORS = C

    -- ui.region(id, x, y, w, h, opts) -> {hover, pressed, released, clicked, held}: a rectangle that takes the
    -- mouse, for widgets of a mod's own.
    function ui.region(id, x, y, w, h, opts)
        opts = opts or {}
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        if uis.modal and uis.modal ~= opts._rml_doc then
            -- A modal RmlUi document is open: everything else takes no mouse and is no pad stop.
            return {hover = false, pressed = false, released = false, clicked = false, held = false, hidden = true}
        end
        local handle = ui_handle(mod, id)
        local c = uis.clip_stack[#uis.clip_stack]
        if c then
            -- Inside a scroll area: only the visible part takes the mouse; nothing if it is scrolled out of sight.
            local x0, y0 = math.max(x, c.x), math.max(y, c.y)
            local x1, y1 = math.min(x + w, c.x + c.w), math.min(y + h, c.y + c.h)
            if x1 <= x0 or y1 <= y0 then
                return {hover = false, pressed = false, released = false, clicked = false, held = false, hidden = true}
            end
            x, y, w, h = x0, y0, x1 - x0, y1 - y0
        end
        ui_count(mod)
        native.ui_region(layer, x, y, w, h, handle)
        local rn = ui_regions.n + 1
        ui_regions.n = rn
        local re = ui_regions[rn]
        if not re then re = {}; ui_regions[rn] = re end
        re.handle, re.layer, re.x, re.y, re.w, re.h, re.id, re.mod = handle, layer, x, y, w, h, id, mod.id
        if opts.nav ~= false and opts.enabled ~= false then
            local nn = ui_nav.n + 1
            ui_nav.n = nn
            local ne = ui_nav[nn]
            if not ne then ne = {}; ui_nav[nn] = ne end
            ne.mod, ne.x, ne.y, ne.w, ne.h, ne.layer = mod, x, y, w, h, layer
            ne.kind, ne.ax, ne.ay, ne.drop = opts._nav_kind, opts._nav_x, opts._nav_y, opts._nav_drop
            ne.native, ne.home = opts._nav_native, opts._nav_home
        end
        local s = ui_input[handle] or uis.NO_INPUT
        local enabled = opts.enabled ~= false
        local r = {
            hover = ui_hot == handle,
            pressed = enabled and s.pressed or false,
            released = s.released or false,
            clicked = enabled and s.clicked or false,
            held = enabled and ui_active == handle,
        }
        if r.hover and opts.tooltip and ui_hover_since[handle] and native.time() - ui_hover_since[handle] >= UI_TOOLTIP_DELAY then
            ui_tooltip = {text = tostring(opts.tooltip):gsub('\\', '/'), mod = mod}
        end
        return r
    end
    local region = ui.region

    ---------------------------------------------------------------------------------------------------------
    -- gear.ui.rml: documents in RmlUi's HTML/CSS dialect (.rml, .rcss), drawn through the overlay and reporting back to Lua.
    -- A document is loaded once, then drawn every frame from the mod's draw handler (`doc:draw()`), which also feeds
    -- it the mouse, the pad (every button, link and input is a pad stop) and the keyboard (text fields), and hands
    -- the events of the listeners registered with `doc:on` to their Lua functions. Files come from the mod's own folder.
    ---------------------------------------------------------------------------------------------------------
    local rml = {}
    ui.rml = rml
    local Doc = {}
    Doc.__index = Doc
    uis.rml_docs = uis.rml_docs or {}
    local RML_KEYS = {'backspace', 'delete', 'enter', 'tab', 'escape', 'left', 'right', 'up', 'down', 'home', 'end', 'pageup', 'pagedown'}
    local RML_CHORDS = {'a', 'c', 'x', 'v'}
    -- The held modifiers as the bits native.rml_key / rml_mouse take: 1 Ctrl, 2 Shift, 4 Alt.
    local function rml_modifiers()
        return (native.key_down(0x11) and 1 or 0) + (native.key_down(0x10) and 2 or 0) + (native.key_down(0x12) and 4 or 0)
    end

    -- The names of the callbacks of data-event-* attributes (opts.calls: name -> function).
    function uis.rml_call_names(opts)
        local names = {}
        for name in pairs(opts.calls or {}) do names[#names + 1] = name end
        table.sort(names)
        return names
    end
    -- The theme mod's folder and stylesheet (a running mod whose mod.lua has `theme = 'file.rcss'`), for the natives; nothing with opts.theme = false.
    function uis.rml_theme(opts)
        if opts.theme == false or not uis.theme_provider then return end
        return uis.theme_provider(opts.dp)
    end
    -- doc.model: the data model's values, read and written like a table; writing a value (at any depth) has the bindings of the
    -- document refresh. The top-level keys are the ones the model was loaded with.
    function uis.rml_proxy(doc, raw, top)
        return setmetatable({}, {
            __index = function(_, k)
                local v = raw[k]
                if type(v) == 'table' then return uis.rml_proxy(doc, v, top or k) end
                return v
            end,
            __newindex = function(_, k, v)
                raw[k] = v
                local key = top or k
                if not doc.closed then native.rml_model_set(doc.id, key, doc.data[key]) end
            end,
        })
    end
    local function rml_open(id, opts)
        local doc = setmetatable({id = id, handlers = {}, opts = opts, calls = opts.calls or {}}, Doc)
        if opts.model then
            doc.data = opts.model
            doc.model = uis.rml_proxy(doc, doc.data)
        end
        uis.rml_docs[id] = {mod = mod, doc = doc}
        -- close buttons: <button id="x" data-close-key="panel"> fires the declared key `panel` of the mod
        doc:on('[data-close-key]', 'click', function(e)
            local key = native.rml_attr(id, '#' .. tostring(e.current), 'data-close-key')
            if type(key) == 'string' and key ~= '' then uis.key_trigger(mod, key) end
        end)
        return doc
    end

    -- rml.load(path, {dp}) -> document | nil, message: path is a .rml file in the mod's folder. With dp = true the document is
    -- laid out in screen pixels with 1dp = one layout unit (text is rasterized for the screen: crisp); otherwise 1px = one layout unit.
    function rml.load(path, opts)
        assert(type(path) == 'string', 'path must be a string')
        if not native.rml_load then error('RmlUi is not available in this build', 2) end
        opts = opts or {}
        local theme_dir, theme_file = uis.rml_theme(opts)
        -- (no theme running and none refused: the plain base style of the host is the look of the document)
        local id, err = native.rml_load(mod.dir, path, opts.dp and true or false, opts.model, uis.rml_call_names(opts), theme_dir, theme_file, theme_dir == nil and opts.theme ~= false)
        if not id then return nil, err end
        if opts.origin == 'screen' and native.rml_origin then native.rml_origin(id, true) end
        return rml_open(id, opts)
    end

    -- rml.load_string(markup, {dp}) -> document | nil, message: the same from a string of RML (relative links resolve in the mod's folder).
    function rml.load_string(markup, opts)
        assert(type(markup) == 'string', 'markup must be a string')
        if not native.rml_load_string then error('RmlUi is not available in this build', 2) end
        opts = opts or {}
        local dir = mod.dir
        if mod.builtin then
            -- A mod built into the DLL has no folder: its inline documents take the theme mod's (they link no files of their own), or a name that is no folder
            -- (there is nothing to read then, and the base style of the host is the look).
            dir = uis.theme_provider and uis.theme_provider() or 'gear'
        end
        local theme_dir, theme_file = uis.rml_theme(opts)
        local id, err = native.rml_load_string(dir, markup, opts.dp and true or false, opts.model, uis.rml_call_names(opts), theme_dir, theme_file, mod.builtin and true or false,
            theme_dir == nil and opts.theme ~= false)
        if not id then return nil, err end
        if opts.origin == 'screen' and native.rml_origin then native.rml_origin(id, true) end
        return rml_open(id, opts)
    end

    -- rml.load_font(path, fallback) -> ok: a TTF / OTF file of the mod, for font-family in its documents (the default face is Segoe UI).
    function rml.load_font(path, fallback)
        assert(type(path) == 'string', 'path must be a string')
        return native.rml_font and native.rml_font(mod.dir, path, fallback and true or false) or false
    end

    -- ui.texture.load(path) -> texture | nil, message: an image file of the mod as a reference counted texture (shared by every mod that asks for the same
    -- file); texture.src is its source in documents, texture:release() lets go. A mod's textures are released when it stops.
    local texture = {}
    ui.texture = texture
    local Texture = {}
    Texture.__index = Texture
    uis.textures = uis.textures or {}
    function texture.load(path)
        assert(type(path) == 'string', 'path must be a string')
        if not native.texture_load then error('RmlUi is not available in this build', 2) end
        local id, w, h = native.texture_load(mod.dir, path)
        if not id then return nil, w end
        local t = setmetatable({id = id, src = 'gear:tex/' .. id, width = w, height = h, released = false, picture = native.texture_picture(id)}, Texture)
        uis.textures[t] = mod
        return t
    end
    function Texture:release()
        if self.released then return end
        self.released = true
        uis.textures[self] = nil
        native.texture_release(self.id)
    end

    -- rml.fonts() -> {default, game, start}, each {family, real, file}: the font families the theme uses (font-family: Sacred Game ...) and whether
    -- each is its real file (the game's ANTQS__.TTF and CAROLING.TTF from the font folder next to Sacred.exe, Segoe UI) or the fallback.
    function rml.fonts() return native.rml_fonts and native.rml_fonts() or nil end
    -- rml.game_font() -> family, real: the game's own font family for font-family, and whether it is the real file.
    function rml.game_font()
        local f = rml.fonts()
        if not f then return 'Segoe UI', false end
        return f.game.family, f.game.real
    end

    local function rml_escape(text)
        return (tostring(text):gsub('&', '&amp;'):gsub('<', '&lt;'):gsub('>', '&gt;'))
    end

    -- doc:on(selector, event, fn) -> ok: fn(event, doc) runs when `event` ('click', 'change', 'submit', 'keydown' ...) reaches an
    -- element matching `selector` ('#id', '.class', 'button' ...; '' is the document), also one added later. event = {type, target (id of the
    -- element hit), current (id of the element listening), params = {name = text}}; a change of a form control carries params.value.
    function Doc:on(selector, event, fn)
        assert(type(fn) == 'function', 'on(selector, event, fn)')
        local listener = native.rml_listen(self.id, tostring(selector), tostring(event))
        if not listener then return false end
        self.handlers[listener] = fn
        return true
    end

    -- Reading and changing the document: `selector` finds the first matching element (nil answers: no such element).
    function Doc:html(selector, markup) return native.rml_html(self.id, selector, markup) end
    function Doc:text(selector, text)
        if text ~= nil then return native.rml_html(self.id, selector, rml_escape(text)) end
        return native.rml_html(self.id, selector)
    end
    -- doc:attr(selector, name [, value]): value true sets a flag attribute (disabled, checked), false removes the attribute.
    function Doc:attr(selector, name, value)
        if type(value) == 'number' then value = tostring(value) end
        return native.rml_attr(self.id, selector, name, value)
    end
    function Doc:class(selector, name, on) return native.rml_class(self.id, selector, name, on and true or false) end
    -- doc:style(selector, property, value): an inline style; nil or false takes it off again.
    function Doc:style(selector, property, value)
        if type(value) == 'number' then value = tostring(value) end
        return native.rml_style(self.id, selector, property, value or false)
    end
    -- doc:value(selector [, text]): the value of a form control (an input, a select, a textarea).
    function Doc:value(selector, text)
        if type(text) == 'number' then text = tostring(text) end
        return native.rml_value(self.id, selector, text)
    end
    -- Elements: doc:query(selector) -> element | nil, doc:query_all(selector) -> {element}. An element is a handle (it is addressed by a
    -- number the host gives it): removing it from the document makes its methods answer nothing, nothing crashes.
    local Element = {}
    Element.__index = Element
    function uis.rml_element(doc, handle)
        return setmetatable({doc = doc, handle = handle, sel = '[data-gh="' .. handle .. '"]'}, Element)
    end
    function Doc:query(selector)
        local h = native.rml_query(self.id, tostring(selector))
        return h and uis.rml_element(self, h) or nil
    end
    function Doc:query_all(selector)
        local out = {}
        for i, h in ipairs(native.rml_query(self.id, tostring(selector), true)) do out[i] = uis.rml_element(self, h) end
        return out
    end
    function Element:exists() return native.rml_tag(self.doc.id, self.sel) ~= nil end
    function Element:tag() return native.rml_tag(self.doc.id, self.sel) end
    function Element:query(selector)
        local h = native.rml_query(self.doc.id, self.sel .. ' ' .. tostring(selector))
        return h and uis.rml_element(self.doc, h) or nil
    end
    function Element:query_all(selector)
        local out = {}
        for i, h in ipairs(native.rml_query(self.doc.id, self.sel .. ' ' .. tostring(selector), true)) do out[i] = uis.rml_element(self.doc, h) end
        return out
    end
    function Element:parent()
        local h = native.rml_relatives(self.doc.id, self.sel, 'parent')
        return h and uis.rml_element(self.doc, h) or nil
    end
    function Element:children()
        local out = {}
        for i, h in ipairs(native.rml_relatives(self.doc.id, self.sel, 'children')) do out[i] = uis.rml_element(self.doc, h) end
        return out
    end
    -- element:append(tag [, {attribute = text}]) -> the new element (the last child), nil if there is no such parent.
    function Element:append(tag, attrs)
        local a
        if attrs then
            a = {}
            for k, v in pairs(attrs) do a[tostring(k)] = tostring(v) end
        end
        local h = native.rml_append(self.doc.id, self.sel, tostring(tag), a)
        return h and h ~= 0 and uis.rml_element(self.doc, h) or nil
    end
    function Element:remove() return native.rml_remove(self.doc.id, self.sel) end
    function Element:html(markup) return self.doc:html(self.sel, markup) end
    function Element:text(text) return self.doc:text(self.sel, text) end
    function Element:attr(name, value) return self.doc:attr(self.sel, name, value) end
    function Element:class(name, on) return self.doc:class(self.sel, name, on) end
    function Element:style(property, value) return self.doc:style(self.sel, property, value) end
    function Element:value(text) return self.doc:value(self.sel, text) end
    function Element:rect() return native.rml_rect(self.doc.id, self.sel) end
    -- element:focus([on]) -> focused: gives the element the focus (a text field then takes the keys); `false` takes it away.
    function Element:focus(on)
        local found, focused = native.rml_focus(self.doc.id, self.sel, on ~= false)
        if not found then return nil end
        return focused
    end
    -- element:scroll([to]) -> top, height, client: the scroll position of a box with overflow: auto / scroll, in layout units, after moving it
    -- when `to` is given: a number (the top of the visible part), 'start' or 'end' (the bottom of the content).
    function Element:scroll(to)
        local info = native.rml_scroll(self.doc.id, self.sel)
        if not info then return nil end
        if to == 'start' then to = 0
        elseif to == 'end' then to = math.max(0, info.height - info.client) end
        if type(to) == 'number' then info = native.rml_scroll(self.doc.id, self.sel, to) end
        return info.top, info.height, info.client
    end
    -- element:on(event, fn): fn(event, element) when the event happens on the element (also for elements added later).
    function Element:on(event, fn)
        assert(type(fn) == 'function', 'on(event, fn)')
        return self.doc:on(self.sel, event, function(e) return fn(e, self) end)
    end

    -- doc:get(key) -> the plain value of a top-level key of the data model (a table for lists and records).
    function Doc:get(key) return self.data and self.data[key] end
    function Doc:show() native.rml_show(self.id, true) end
    -- A document that is not on screen has no focus: nothing of it is focused or captures the keyboard (also when it just stops being drawn, see uis.rml_release_idle).
    function Doc:release_focus()
        if self.closed then return end
        if native.rml_blur then native.rml_blur(self.id) end
        if self.capturing then
            local entry = uis.rml_docs[self.id]
            if entry then key_capture[entry.mod] = nil end
            self.capturing = false
        end
        self.idle_released = true
    end
    function Doc:hide()
        native.rml_show(self.id, false)
        self:release_focus()
    end

    function Doc:close()
        if self.closed then return end
        self.closed = true
        uis.rml_docs[self.id] = nil
        native.rml_close(self.id)
        if self.capturing then ui.capture_keys(false) end
    end

    -- doc:draw({layer}) -> commands drawn: call it every frame from a draw handler. It lets the document take the mouse where it has
    -- elements, makes every button, link and input a pad stop, reads the keys for a focused text field, runs the document's
    -- animations, calls the handlers of events that happened and draws it.
    function Doc:draw(opts)
        if self.closed then return 0 end
        self.drawn_frame = uis.frame
        self.idle_released = false
        opts = opts or {}
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        local id = self.id
        local mx, my = ui.mouse()
        if not self.primed then
            -- Lay the document out once, so its windows have their places before the first frame is drawn.
            self.primed = true
            native.rml_update(id, mx, my, false)
        end
        do
            -- The classes of <body> that tell the stylesheet whether and which button prompts to show: "pad" while the
            -- controller was used last, "pad-ps" or "pad-switch" for the style of the pad.
            local on, style = false, 0
            if native.ui_pad_info then on, style = native.ui_pad_info() end
            local key = (on and 'pad' or '') .. ':' .. tostring(style)
            if self.pad_key ~= key then
                self.pad_key = key
                native.rml_class(id, 'body', 'pad', on)
                native.rml_class(id, 'body', 'pad-ps', style == 1)
                native.rml_class(id, 'body', 'pad-switch', style == 2)
            end
        end
        local hover = false
        local user_range = false
        -- Per frame nothing is allocated here: the option tables and the region keys are kept in the document, the rows come back into
        -- the same tables.
        local ropts = self.ropts
        if not ropts or ropts.layer ~= opts.layer then
            ropts = {layer = opts.layer, nav = false, _rml_doc = id}
            self.ropts = ropts
            self.nopts = {layer = opts.layer, _rml_doc = id, _nav_native = true}
            self.keys = {}
        end
        local keys = self.keys
        if opts.modal and not ui_edit then
            -- A modal dialog: it blocks the mouse and the pad for the game and for every other overlay until it is closed.
            uis.modal_next = id
            local sl, st, sr, sb = screen_rect()
            region('rml:' .. id .. ':modal', sl, st, sr - sl, sb - st, ropts)
            hover = true
        end
        local panels = self.panels or {}
        self.panels = panels
        local rects = native.rml_rects(id, self.rect_rows)
        self.rect_rows = rects
        for i, r in ipairs(rects) do
            if r.panel and r.panel ~= '' then
                -- A window the player may move (data-panel="name"): a panel of the layout like the ones of gear.ui.panel, so the layout
                -- editor lists it and keeps its place (mods\gear_layout.json, key "<mod id>:<name>").
                local key = mod.id .. ':' .. r.panel
                local p = panels[r.panel]
                if not p then p = {x = r.x, y = r.y}; panels[r.panel] = p end      -- where the document puts it
                local x, y = p.x, p.y
                local saved = layout_get()[key]
                if saved and saved.x then x, y = from_virtual(saved.x, saved.y) end
                if p.drag_x then x, y = p.drag_x, p.drag_y end       -- being dragged by its title bar
                local sel = '[data-panel="' .. r.panel .. '"]'
                if r.resizable then
                    -- data-resizable: the layout editor sizes it (a corner grip, or the popup's numbers); the size is kept like the place
                    local w, h = r.w, r.h
                    if saved and saved.w then w, h = size_from_virtual(saved.w, saved.h) end
                    w, h = math.max(w, r.min_width or 120), math.max(h, r.min_height or 60)
                    if math.abs(w - r.w) > 0.25 or math.abs(h - r.h) > 0.25 then
                        if native.rml_resize(id, sel, w, h) then r.w, r.h = w, h end
                    end
                end
                local sl, st, sr, sb = screen_rect()
                x, y = math.max(sl, math.min(x, sr - r.w)), math.max(st, math.min(y, sb - r.h))
                if math.abs(x - r.x) > 0.25 or math.abs(y - r.y) > 0.25 then
                    if native.rml_move(id, sel, x, y) then r.x, r.y = x, y end
                end
                ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = r.panel, title = r.title ~= '' and r.title or r.panel,
                    x = r.x, y = r.y, w = r.w, h = r.h, closable = false, back = false, resizable = r.resizable and true or false,
                    min_width = r.min_width or 0, min_height = r.min_height or 0, layer = opts.layer or 'hud'}
                if ui_edit then ui.frame(r.x, r.y, r.w, r.h, C.accent, 1, {layer = opts.layer}) end
            end
            -- The game's own frame behind a window with class "window" or data-frame (the ornaments overhang the box, so it cannot be a CSS decorator).
            if r.frame and r.frame ~= '' and ui.native then
                local divs
                if r.dividers and r.dividers ~= '' then
                    divs = {}
                    for n in r.dividers:gmatch('[%d%.]+') do divs[#divs + 1] = tonumber(n) end
                end
                local fi
                if r.fill_inset and r.fill_inset ~= '' then
                    local n = {}
                    for v in r.fill_inset:gmatch('[%d%.]+') do n[#n + 1] = tonumber(v) end
                    fi = #n == 1 and n[1] or (#n >= 4 and n or nil)
                end
                ui.native.frame(r.x, r.y, r.w, r.h, {style = r.frame, layer = opts.layer, dividers = divs, fill_inset = fi})
            end
            -- In the layout editor the windows take no mouse: the editor drags them.
            local wkey = keys[-i]
            if not wkey then wkey = 'rml:' .. id .. ':w' .. i; keys[-i] = wkey end
            if not ui_edit and region(wkey, r.x, r.y, r.w, r.h, ropts).hover then hover = true end
            if not ui_edit and r.panel and r.panel ~= '' and (r.title_h or 0) > 0 then
                -- A window with a title bar: the player drags it by the bar (registered after the window's region and before the buttons', so a
                -- close button in the bar still takes its click). The place is kept in the layout like the layout editor's (key "<mod id>:<panel>").
                local tkey = keys[-100000 - i]
                if not tkey then tkey = 'rml:' .. id .. ':t' .. i; keys[-100000 - i] = tkey end
                local topts = self.topts
                if not topts then topts = {layer = opts.layer, _rml_doc = id, nav = false}; self.topts = topts end
                local bar = region(tkey, r.x + r.title_x, r.y + r.title_y, r.title_w, r.title_h, topts)
                if bar.hover then hover = true end
                local p = panels[r.panel]
                if bar.pressed then
                    p.grab_x, p.grab_y, p.press_x, p.press_y = mx - r.x, my - r.y, mx, my
                end
                if p.grab_x then
                    if bar.held then
                        -- a click on the bar moves nothing; only a drag (a few units) does
                        if p.drag_x or math.abs(mx - p.press_x) + math.abs(my - p.press_y) >= 3 then
                            local sl, st, sr, sb = screen_rect()
                            p.drag_x = math.max(sl, math.min(mx - p.grab_x, sr - r.w))
                            p.drag_y = math.max(st, math.min(my - p.grab_y, sb - r.h))
                        end
                        hover = true
                    else
                        if p.drag_x then
                            local vx, vy = to_virtual(p.drag_x, p.drag_y)
                            local e = {x = vx, y = vy}
                            local old = layout_get()[mod.id .. ':' .. r.panel]
                            if old and old.w then e.w, e.h = old.w, old.h end
                            layout_set(mod.id .. ':' .. r.panel, e)
                        end
                        p.grab_x, p.grab_y, p.drag_x, p.drag_y = nil, nil, nil, nil
                    end
                end
            end
        end
        if not ui_edit then
            -- Lists marked data-nav-scroll are the pad's scroll areas (up / down walks their cards; at the edge of the view the list scrolls to the next one).
            local areas = native.rml_scrolls(id, self.scroll_rows)
            self.scroll_rows = areas
            local akeys = self.area_keys
            if not akeys then
                akeys = {}
                self.area_keys = akeys
                self.area_opts = {}
            end
            for i, r in ipairs(areas) do
                local akey = akeys[i]
                if not akey then akey = 'rml:' .. id .. ':a' .. i; akeys[i] = akey end
                -- kind 2 is a scroll area; + 4 at its top, + 8 at its bottom (the pad leaves the list there)
                local kind = 2 + (r.at_top and 4 or 0) + (r.at_bottom and 8 or 0)
                local aopts = self.area_opts[kind]
                if not aopts then aopts = {layer = opts.layer, _rml_doc = id, _nav_kind = kind}; self.area_opts[kind] = aopts end
                local ares = region(akey, r.x, r.y, r.w, r.h, aopts)
                if ares.hover then hover = true end
            end
            -- The scroll bars of the boxes that scroll take the real mouse: a press on the knob, the groove or an arrow is RmlUi's own (a drag goes on, the groove
            -- pages toward the cursor), as for a range input below.
            local bars = native.rml_scrollbars(id, self.bar_rows)
            self.bar_rows = bars
            local bkeys = self.bar_keys
            if not bkeys then bkeys = {}; self.bar_keys = bkeys end
            for i, r in ipairs(bars) do
                local bkey = bkeys[i]
                if not bkey then bkey = 'rml:' .. id .. ':b' .. i; bkeys[i] = bkey end
                local bres = region(bkey, r.x, r.y, r.w, r.h, self.ropts)
                if bres.hover then hover = true end
                if bres.pressed then
                    native.rml_mouse(id, 'down', ui_mouse.x, ui_mouse.y)
                    self.bar_down = i
                elseif self.bar_down == i then
                    if bres.held then
                        hover = true
                    else
                        native.rml_mouse(id, 'up', ui_mouse.x, ui_mouse.y)
                        self.bar_down = nil
                    end
                end
            end
            if self.bar_down and self.bar_down > #bars then
                native.rml_mouse(id, 'up', ui_mouse.x, ui_mouse.y)
                self.bar_down = nil
            end
            if #areas > 0 then
                if uis.pad_scroll ~= 0 and native.rml_pad_scroll(id, mx, my, uis.pad_scroll) then uis.pad_scroll = 0 end
                -- with the controller: the card under its cursor is brought fully into view (the pad's step puts the cursor on a card whose centre is in view)
                if native.ui_pad_info and native.ui_pad_info() then native.rml_pad_reveal(id, mx, my) end
            end
            local stops = native.rml_nav(id, self.nav_rows)
            self.nav_rows = stops
            for i, r in ipairs(stops) do
                local nkey = keys[i]
                if not nkey then nkey = 'rml:' .. id .. ':n' .. i; keys[i] = nkey end
                local homeopts = self.homeopts
                if not homeopts then homeopts = {layer = opts.layer, _rml_doc = id, _nav_native = true, _nav_home = true}; self.homeopts = homeopts end
                local res = region(nkey, r.x, r.y, r.w, r.h, r.home and homeopts or self.nopts)
                if res.hover then hover = true end
                if r.range then
                    -- A slider takes the real mouse: a press jumps to that value and a drag goes on (RmlUi's own slider does that), the
                    -- minus and plus buttons step; the pad's D-pad nudges it.
                    if res.pressed then
                        native.rml_mouse(id, 'down', ui_mouse.x, ui_mouse.y)
                        self.range_down = i
                        user_range = true
                    elseif self.range_down == i then
                        user_range = true
                        if res.held then
                            hover = true
                        else
                            native.rml_mouse(id, 'up', ui_mouse.x, ui_mouse.y)
                            self.range_down = nil
                        end
                    end
                    if res.hover and not res.held and ui_pad.nudge ~= 0 then
                        user_range = true
                        native.rml_nudge(id, i, ui_pad.nudge)
                        ui_pad.nudge = 0
                    end
                elseif r.text and not (native.ui_pad_info and native.ui_pad_info()) then
                    -- A text field takes the real mouse (not the pad's A: that keeps the click at its centre): a press puts the caret (Shift extends
                    -- the selection), a drag selects, a double click takes the word under the cursor (RmlUi's own text widget does all that).
                    if res.pressed then
                        native.rml_mouse(id, 'down', ui_mouse.x, ui_mouse.y, rml_modifiers())
                        self.text_down = i
                        hover = true
                    elseif self.text_down == i then
                        if res.held then
                            hover = true
                        else
                            native.rml_mouse(id, 'up', ui_mouse.x, ui_mouse.y)
                            self.text_down = nil
                        end
                    end
                elseif res.clicked then native.rml_click(id, i) end
            end
            if self.text_down and self.text_down > #stops then
                native.rml_mouse(id, 'up', ui_mouse.x, ui_mouse.y)
                self.text_down = nil
            end
        end
        if native.rml_wants_text(id) then
            ui.capture_keys(true)
            self.capturing = true
            local typed = ui.typed()
            local mods = rml_modifiers()
            -- With Ctrl held (and not AltGr) the keyboard's characters are the chord's letter: they are no text (a player's Ctrl+A is the control character,
            -- which ui.typed drops; automation's injected keys are translated without the Ctrl state and would type it)
            if typed ~= '' and not (bit.band(mods, 1) ~= 0 and bit.band(mods, 4) == 0) then native.rml_text(id, typed) end
            for _, name in ipairs(RML_KEYS) do
                if ui.key_pressed(name) then
                    native.rml_key(id, UI_VK[name], true, mods)
                    native.rml_key(id, UI_VK[name], false, mods)
                end
            end
            -- Ctrl + A / C / X / V: select all, copy, cut, paste (RmlUi's text widget through the Windows clipboard); not with AltGr (Ctrl + Alt)
            for _, name in ipairs(RML_CHORDS) do
                if ui.key_pressed(name) and bit.band(mods, 1) ~= 0 and bit.band(mods, 4) == 0 then
                    native.rml_key(id, UI_VK[name], true, mods)
                    native.rml_key(id, UI_VK[name], false, mods)
                end
            end
        elseif self.capturing then
            ui.capture_keys(false)
            self.capturing = false
        end
        if hover and uis.wheel ~= 0 and not uis.wheel_taken then
            uis.wheel_taken = true
            native.rml_wheel(id, uis.wheel)
        end
        if ui_pad.back > 0 then
            -- the pad's B closes the window under its cursor that has a close button (data-close-key)
            local key = native.rml_attr(id, '[data-close-key]', 'data-close-key')
            if type(key) == 'string' and key ~= '' then
                for _, r in ipairs(native.rml_rects(id, self.rect_rows) or {}) do
                    if mx >= r.x and mx <= r.x + r.w and my >= r.y and my <= r.y + r.h then
                        ui_pad.back = ui_pad.back - 1
                        uis.key_trigger(mod, key)
                        break
                    end
                end
            end
        end
        for _, e in ipairs(native.rml_update(id, mx, my, hover)) do
            -- e.user: a slider's change that the player made (mouse press / drag / release, or the pad's D-pad) this frame; RmlUi also
            -- reports a change when the document itself sets a slider's value (data binding), which a handler must not take as a choice.
            if e.type == 'change' then e.user = user_range end
            local fn = e.call and self.calls[e.call] or self.handlers[e.listener]
            if e.call and fn then
                local ok, err = xpcall(fn, traceback, e.args, self, e)
                if not ok then log('[%s] rml callback %s failed: %s', mod.id, e.call, tostring(err)) end
                fn = nil
            end
            if fn then
                local ok, err = xpcall(fn, traceback, e, self)
                if not ok then log('[%s] rml handler for %s failed: %s', mod.id, e.type, tostring(err)) end
            end
            if self.closed then return 0 end
        end
        if native.rml_reloads then
            -- Hot reload (a developer's switch): the document was loaded again from its changed files. What Lua changed in it by hand (doc:text,
            -- doc:class, doc:style ...) is gone: opts.on_reload(doc, err) puts it back; err is the last failed reload's message.
            local n, err = native.rml_reloads(id)
            if n ~= (self.reloads_seen or 0) then
                self.reloads_seen = n
                if self.opts.on_reload then
                    local ok, e2 = xpcall(self.opts.on_reload, traceback, self, err)
                    if not ok then log('[%s] rml on_reload failed: %s', mod.id, tostring(e2)) end
                end
            end
        end
        if self.data then
            -- A value the document changed itself (data-value): into the table, and the mod's on_change.
            local changed = native.rml_model_changed(id)
            if changed then
                for k, v in pairs(changed) do self.data[k] = v end
                if self.opts.on_change then
                    local ok, err = xpcall(self.opts.on_change, traceback, self, self.data)
                    if not ok then log('[%s] rml on_change failed: %s', mod.id, tostring(err)) end
                end
            end
        end
        local n = native.rml_draw(id, layer, -1)
        if n > 0 then ui_count(mod, n) end
        return n
    end

    -- The cursor in layout coordinates, and whether the left button is held over a mod's widget.
    function ui.mouse() return ui_mouse.x, ui_mouse.y end
    -- ui.over_ui(): whether the cursor is over a region of a mod's UI (as of the last frame).
    function ui.over_ui() return ui_hot ~= nil end

    -- ui.key_pressed(name): true once per press of the key ('f9', 'a', 'escape' ...), while the game has the focus.
    function ui.key_pressed(name)
        local vk = type(name) == 'number' and name or UI_VK[tostring(name):lower()]
        if not vk then error('unknown key ' .. tostring(name), 2) end
        local keys = ui_keys[mod] or {}
        ui_keys[mod] = keys
        local down = native.key_down(vk)
        local was = keys[vk]
        keys[vk] = down
        return down and not was
    end

    local scrolls = {}      -- scroll area id -> {offset, content, ...} (ui.scroll_begin), for the session
    local function box(x, y, w, h, fill, edge, opts)
        ui.rect(x, y, w, h, fill, opts)
        ui.frame(x, y, w, h, edge, 1, opts)
    end
    local function label_color(opts) return opts.enabled == false and C.text_dim or (opts.color or C.text) end

    -- ui.label(text, x, y, opts): text with the widgets' look (the UI font unless opts.font), a tooltip if opts.tooltip.
    function ui.label(id, text, x, y, opts)
        opts = opts or {}
        local w, h = ui_measure(tostring(text), opts)
        if opts.tooltip then region(id, x, y, w or 60, h or 14, {tooltip = opts.tooltip, layer = opts.layer, nav = false}) end
        return ui.text(text, x, y, {font = opts.font, size = opts.size, face = opts.face, bold = opts.bold,
            italic = opts.italic, height = opts.height, valign = opts.valign, color = label_color(opts),
            layer = opts.layer})
    end

    -- ui.button(id, x, y, w, h, text, opts) -> clicked
    function ui.button(id, x, y, w, h, text, opts)
        opts = opts or {}
        local r = region(id, x, y, w, h, opts)
        local fill = (r.held and C.bg_down) or (r.hover and C.bg_hover) or C.bg
        box(x, y, w, h, fill, r.hover and C.edge or C.edge_dim, opts)
        if opts.image then
            ui.image(opts.image, x + 2, y + 2, h - 4, h - 4, {fit = 'contain', layer = opts.layer})
        end
        ui.text(text, x, y, {width = w, height = h, align = 'center', valign = 'middle',
            color = label_color(opts), layer = opts.layer})
        return r.clicked
    end

    -- ui.checkbox(id, x, y, text, checked, opts) -> changed, checked
    function ui.checkbox(id, x, y, text, checked, opts)
        opts = opts or {}
        local tw = ui_measure(tostring(text), {}) or 60
        local r = region(id, x, y, 18 + tw, 16, opts)
        box(x, y + 1, 14, 14, r.hover and C.bg_hover or C.bg, r.hover and C.edge or C.edge_dim, opts)
        if checked then ui.rect(x + 3, y + 4, 8, 8, opts.enabled == false and C.text_dim or C.accent, opts) end
        ui.text(text, x + 20, y, {height = 16, valign = 'middle', color = label_color(opts), layer = opts.layer})
        if r.clicked then return true, not checked end
        return false, checked
    end

    -- A round mark of radius 6 from rectangles (the overlay has no circles).
    local function disc(cx, cy, rad, color, opts)
        for dy = -rad, rad - 1 do
            local yy = dy + 0.5
            local half = math.floor(math.sqrt(math.max(0, rad * rad - yy * yy)) + 0.5)
            ui.rect(cx - half, cy + dy, half * 2, 1, color, opts)
        end
    end

    -- ui.radio(id, x, y, text, selected, opts) -> clicked
    function ui.radio(id, x, y, text, selected, opts)
        opts = opts or {}
        local tw = ui_measure(tostring(text), {}) or 60
        local r = region(id, x, y, 18 + tw, 16, opts)
        disc(x + 7, y + 8, 7, r.hover and C.edge or C.edge_dim, opts)
        disc(x + 7, y + 8, 6, r.hover and C.bg_hover or 0xF0141420, opts)
        if selected then disc(x + 7, y + 8, 3, opts.enabled == false and C.text_dim or C.accent, opts) end
        ui.text(text, x + 20, y, {height = 16, valign = 'middle', color = label_color(opts), layer = opts.layer})
        return r.clicked
    end

    -- ui.radio_group(id, x, y, options, index, opts{horizontal, spacing}) -> changed, index
    function ui.radio_group(id, x, y, options, index, opts)
        opts = opts or {}
        local changed = false
        local px, py = x, y
        for i, text in ipairs(options) do
            local o = {tooltip = opts.tooltips and opts.tooltips[i], layer = opts.layer, enabled = opts.enabled}
            if ui.radio(tostring(id) .. '#' .. i, px, py, text, i == index, o) and i ~= index then
                index, changed = i, true
            end
            if opts.horizontal then
                px = px + 26 + (ui_measure(tostring(text), {}) or 60) + (opts.spacing or 8)
            else
                py = py + (opts.spacing or 18)
            end
        end
        return changed, index
    end


    -- Scroll areas. ui.scroll_begin(id, x, y, w, h, opts{step, layer, nav}) -> area {x, y, w, h, offset, max,
    -- content_x, content_y}; draw the content at content_y + its own y (what lies outside the viewport is clipped and
    -- takes no mouse), then ui.scroll_end(content_height) -> offset, max. The scrollbar (arrows, track, thumb) shows
    -- when the content is taller than the area; the wheel scrolls the area under the cursor, the keys (Up, Down,
    -- PageUp, PageDown, Home, End) the one under the cursor or clicked last, the D-pad (SacredBild) the one the
    -- pad's cursor is in. The offset is kept per id for the session; ui.scroll_to(id, offset) sets it.
    local scroll_stack = {}
    local function scroll_key_pressed(st, name)
        local vk = UI_VK[name]
        local down = native.key_down(vk)
        st.keys = st.keys or {}
        local was = st.keys[name]
        st.keys[name] = down
        return down and not was
    end
    function ui.scroll_begin(id, x, y, w, h, opts)
        opts = opts or {}
        local key = mod.id .. '\0' .. tostring(id)
        local st = scrolls[id] or {offset = 0, content = 0}
        scrolls[id] = st
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        local step = opts.step or 20
        local bar = st.content > h and uis.SCROLLBAR or 0
        -- The body takes clicks on empty space (they focus the area) and is the pad's stop for it.
        local body_kind = 2 + (st.offset <= 0.5 and 4 or 0) + (st.offset >= st.content - h - 0.5 and 8 or 0)     -- + 4 at the top, + 8 at the bottom: the pad leaves the list there
        local body = region(tostring(id) .. '#body', x, y, w, h, {layer = opts.layer, nav = opts.nav, _nav_kind = body_kind})
        if body.pressed then uis.scroll_focus = key end
        local delta = 0
        local hot = uis.scroll_hot == key
        if hot and uis.wheel ~= 0 and not uis.wheel_taken then
            delta = delta - uis.wheel * step * 3
            uis.wheel_taken = true
        end
        if (hot or uis.scroll_focus == key) and uis.pad_scroll ~= 0 then
            delta = delta + uis.pad_scroll * step
            uis.pad_scroll = 0
        end
        if hot or uis.scroll_focus == key then
            if scroll_key_pressed(st, 'down') then delta = delta + step end
            if scroll_key_pressed(st, 'up') then delta = delta - step end
            if scroll_key_pressed(st, 'pagedown') then delta = delta + h - step end
            if scroll_key_pressed(st, 'pageup') then delta = delta - h + step end
            if scroll_key_pressed(st, 'home') then delta = -math.huge end
            if scroll_key_pressed(st, 'end') then delta = math.huge end
        end
        local max
        st.offset, max = uis.scroll_clamp(st.offset + delta, st.content, h)
        do
            local sn = uis.scroll_now.n + 1
            uis.scroll_now.n = sn
            local a = uis.scroll_now[sn]
            if not a then a = {}; uis.scroll_now[sn] = a end
            a.key, a.x, a.y, a.w, a.h, a.layer = key, x, y, w, h, layer
        end
        local view = {x = x, y = y, w = w - bar, h = h}
        local outer = uis.clip_stack[#uis.clip_stack]
        local c = view
        if outer then
            local x0, y0 = math.max(view.x, outer.x), math.max(view.y, outer.y)
            local x1, y1 = math.min(view.x + view.w, outer.x + outer.w), math.min(view.y + view.h, outer.y + outer.h)
            c = {x = x0, y = y0, w = math.max(0, x1 - x0), h = math.max(0, y1 - y0)}
        end
        uis.clip_stack[#uis.clip_stack + 1] = c
        uis.set_clip(c)
        st.view, st.step, st.layer_name, st.id, st.full_w = view, step, opts.layer, id, w
        scroll_stack[#scroll_stack + 1] = st
        return {x = x, y = y, w = view.w, h = h, offset = st.offset, max = max, content_x = x, content_y = y - st.offset}
    end
    function ui.scroll_end(content_height)
        local st = table.remove(scroll_stack)
        if not st then error('scroll_end without scroll_begin', 2) end
        table.remove(uis.clip_stack)
        uis.set_clip(uis.clip_stack[#uis.clip_stack])
        if content_height then st.content = math.max(0, content_height) end
        local v = st.view
        local max
        st.offset, max = uis.scroll_clamp(st.offset, st.content, v.h)
        if max <= 0 then return st.offset, max end
        local o = {layer = st.layer_name}
        local id = tostring(st.id)
        local bx, bw = v.x + st.full_w - uis.SCROLLBAR + 1, uis.SCROLLBAR - 1
        local track_y, track_h = v.y + bw, v.h - 2 * bw
        local up = region(id .. '#up', bx, v.y, bw, bw, o)
        local down = region(id .. '#down', bx, v.y + v.h - bw, bw, bw, o)
        local track = region(id .. '#track', bx, track_y, bw, track_h, {layer = st.layer_name, nav = false})
        local size, pos = uis.scroll_thumb(st.offset, st.content, v.h, track_h)
        local thumb = region(id .. '#thumb', bx, track_y + pos, bw, size, {layer = st.layer_name, nav = false})
        local now = native.time()
        local function repeating(r)
            if r.pressed then st.repeat_at = now + 0.4; return true end
            if r.held and st.repeat_at and now >= st.repeat_at then st.repeat_at = now + 0.06; return true end
            return false
        end
        local offset = st.offset
        if repeating(up) then offset = offset - st.step end
        if repeating(down) then offset = offset + st.step end
        if thumb.pressed then st.grab = ui_mouse.y - (track_y + pos) end
        if thumb.held and st.grab then
            offset = uis.scroll_from_thumb(ui_mouse.y - st.grab - track_y, st.content, v.h, track_h, size)
        elseif not thumb.held then
            st.grab = nil
        end
        if track.clicked then offset = offset + (ui_mouse.y < track_y + pos and -v.h or v.h) end
        st.offset = uis.scroll_clamp(offset, st.content, v.h)
        size, pos = uis.scroll_thumb(st.offset, st.content, v.h, track_h)
        ui.rect(bx, v.y, bw, v.h, 0xC00C0C14, o)
        box(bx, track_y + pos, bw, size, (thumb.hover or thumb.held) and C.bg_down or C.bg_hover, C.edge, o)
        for _, a in ipairs({{up, v.y, -1}, {down, v.y + v.h - bw, 1}}) do
            local r, ay, dir = a[1], a[2], a[3]
            box(bx, ay, bw, bw, (r.hover or r.held) and C.bg_hover or C.bg, C.edge_dim, o)
            for i = 0, 2 do
                local row = dir < 0 and (2 - i) or i
                ui.rect(bx + 2 + i, ay + 4 + row, bw - 4 - 2 * i, 1, C.accent, o)
            end
        end
        return st.offset, max
    end
    function ui.scroll_to(id, offset)
        local st = scrolls[id] or {offset = 0, content = 0}
        scrolls[id] = st
        st.offset = math.max(0, offset or 0)
    end
    function ui.scroll_offset(id)
        local st = scrolls[id]
        return st and st.offset or 0, st and math.max(0, st.content - (st.view and st.view.h or 0)) or 0
    end

    -- ui.wheel(): wheel notches over mod UI this tick (+ away from the player, as Windows has it; 0 if none), and
    -- whether a scroll area already took them.
    function ui.wheel() return uis.wheel, uis.wheel_taken end

    -- ui.text_wrap(text, width, opts) -> lines, height: the text broken into lines that fit `width` (at spaces; a
    -- word longer than the width gets a line of its own; \n breaks too), and their height, in the font of `opts`
    -- (ui.text's). Cached per text, width and font.
    local wrap_cache = setmetatable({}, {__mode = 'k'})
    local wrap_count = 0
    function ui.text_wrap(text, width, opts)
        opts = opts or {}
        text = tostring(text)
        local fkey = table.concat({tostring(opts.font), tostring(opts.size), tostring(opts.face), tostring(opts.bold),
            tostring(opts.italic)}, '|')
        local key = text .. '\0' .. width .. '\0' .. fkey
        local hit = wrap_cache[key]
        if hit then return hit.lines, hit.height end
        local _, line_h = ui_measure('Ag', opts)
        line_h = line_h or 16
        local lines = {}
        for para in (text .. '\n'):gmatch('(.-)\n') do
            local line = nil
            for word in para:gmatch('%S+') do
                local try = line and (line .. ' ' .. word) or word
                local tw = ui_measure(try, opts) or (#try * 7)
                if tw <= width or not line then
                    line = try
                else
                    lines[#lines + 1] = line
                    line = word
                end
            end
            lines[#lines + 1] = line or ''
        end
        local height = #lines * line_h
        wrap_count = wrap_count + 1
        if wrap_count > 2000 then wrap_cache, wrap_count = setmetatable({}, {__mode = 'k'}), 0 end
        wrap_cache[key] = {lines = lines, height = height}
        return lines, height
    end

    -- ui.dropdown(id, x, y, w, options, index, opts{max_rows}) -> changed, index. The open list is drawn after every
    -- draw handler, over all panels and text, on the top layer; it shows at most max_rows (12) entries and scrolls; a
    -- click anywhere else closes it.
    function ui.dropdown(id, x, y, w, options, index, opts)
        opts = opts or {}
        local h = math.max(14, opts.height or 18)
        local handle = ui_handle(mod, id)
        local open = ui_dropdown == handle
        local r = region(id, x, y, w, h, opts)
        box(x, y, w, h, (r.hover or open) and C.bg_hover or C.bg, (r.hover or open) and C.edge or C.edge_dim, opts)
        ui.text(tostring(options[index] or ''), x + 6, y, {height = h, valign = 'middle', color = label_color(opts),
            layer = opts.layer})
        -- the arrow
        local ay = y + math.floor((h - 4) / 2)
        for i = 0, 3 do ui.rect(x + w - 14 + i, ay + i, 8 - 2 * i, 1, C.accent, opts) end
        if r.clicked then
            open = not open
            ui_dropdown = open and handle or nil
        end
        local n = #options
        local rows = math.max(1, math.min(n, opts.max_rows or 12))
        local list_id = tostring(id) .. '#list'
        if r.clicked and open then
            -- Opened: the selected entry in the middle of the list (when it has to scroll).
            local off = uis.scroll_clamp((index - 1) * 18 - (rows * 18 - 18) / 2, n * 18, rows * 18)
            scrolls[list_id] = {offset = off, content = n * 18}
        end
        local changed = false
        if open then
            -- The list's clicks (its regions were drawn last tick), so the new index is returned now.
            for i = 1, n do
                local st = ui_input[ui_handle(mod, tostring(id) .. '#' .. i)]
                if st and st.clicked then
                    changed = i ~= index
                    index = i
                    ui_dropdown = nil
                end
            end
            local away = ui_input[ui_handle(mod, tostring(id) .. '#away')]
            if away and away.clicked then ui_dropdown = nil end
        end
        if open and ui_dropdown == handle then
            -- The list itself after every draw handler: over all panels and text, and it takes the mouse first.
            local sel = index
            uis.defer(function()
                local s = ui_tick_state or {}
                local top = {layer = 'top'}
                local sl, st_, sr, sb = s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768
                -- Behind the list: the whole screen, so a click beside it closes it (and goes nowhere else).
                region(tostring(id) .. '#away', sl, st_, sr - sl, sb - st_, {layer = 'top', nav = false})
                local lh = rows * 18 + 2
                local ly = y + h
                if ly + lh > sb and y - lh >= st_ then ly = y - lh end     -- no room below: above the box
                box(x, ly, w, lh, 0xF0141420, C.edge, top)
                local area = ui.scroll_begin(list_id, x + 1, ly + 1, w - 2, rows * 18, {layer = 'top', step = 18, nav = false})
                local first = math.max(1, math.floor(area.offset / 18) + 1)
                for i = first, math.min(n, first + rows) do
                    local iy = area.content_y + (i - 1) * 18
                    local ir = region(tostring(id) .. '#' .. i, x + 1, iy, area.w, 18, {layer = 'top', _nav_drop = true})
                    if ir.hover or i == sel then
                        ui.rect(x + 1, iy, area.w, 18, ir.hover and 0xFF4A3C28 or 0xFF2A2418, top)
                    end
                    ui.text(tostring(options[i]), x + 6, iy, {height = 18, valign = 'middle', layer = 'top',
                        color = i == sel and C.accent or C.text})
                end
                ui.scroll_end(n * 18)
            end)
        end
        return changed, index
    end

    -- ui.slider(id, x, y, w, min, max, value, opts{step, format}) -> changed, value
    function ui.slider(id, x, y, w, min, max, value, opts)
        opts = opts or {}
        local function at(v) return x + (w - 10) * ((v - min) / math.max(max - min, 1e-9)) end
        local r = region(id, x, y, w, 16, {layer = opts.layer, tooltip = opts.tooltip, enabled = opts.enabled,
            _nav_kind = 1, _nav_x = at(value) + 5, _nav_y = y + 8})
        ui.rect(x, y + 7, w, 2, C.edge_dim, opts)
        local changed = false
        if ui_hot == ui_handle(mod, id) and ui_pad.nudge ~= 0 and opts.enabled ~= false then
            -- The pad: D-pad left / right on the slider, a fiftieth of its range (at least one step) at a time.
            local step = opts.step or 1
            local jump = math.max(step, math.floor((max - min) / 50 / step + 0.5) * step)
            local v = math.max(min, math.min(max, value + ui_pad.nudge * jump))
            v = math.floor(v / step + 0.5) * step
            if v ~= value then value, changed = v, true end
            ui_pad.nudge = 0
        end
        if r.held or r.pressed then
            local t = math.max(0, math.min(1, (ui_mouse.x - x - 5) / math.max(w - 10, 1)))
            local v = min + t * (max - min)
            local step = opts.step or 1
            v = math.floor(v / step + 0.5) * step
            if v ~= value then value, changed = v, true end
        end
        ui.rect(x, y + 7, at(value) - x + 5, 2, C.accent, opts)
        box(at(value), y + 2, 10, 12, (r.hover or r.held) and C.bg_down or C.bg_hover, C.edge, opts)
        if opts.format ~= false then
            ui.text((opts.format or '%g'):format(value), x + w + 6, y, {height = 16, valign = 'middle',
                color = label_color(opts), layer = opts.layer})
        end
        return changed, value
    end

    do
        ---------------------------------------------------------------------------------------------------------
        -- Tables: ui.table(id, x, y, w, opts{columns, spacing, row_height, row_heights, stripes, borders, header_height,
        -- layer}) -> table. Pure layout on top of the widgets: it resolves the columns' widths (fixed numbers, 'auto' = as wide
        -- as the widest cell of the last frame, shares of the rest with min / max), then t:row() starts a row and t:label /
        -- t:button / t:dropdown / t:checkbox / t:slider / t:image / t:cell put one thing into a column of it (spanning columns
        -- and rows, aligned and stretched as the options say). The widgets are the ordinary ones (same look, same controller
        -- stops); nothing is retained but the measures 'auto' needs, so draw it every frame and inside a scroll area use
        -- area.content_x / content_y.
        ---------------------------------------------------------------------------------------------------------
        local Table = {}
        Table.__index = Table
        local TABLE_ELLIPSIS = '...'
        local table_fit_cache, table_fit_n = {}, 0
        local table_auto = {}       -- mod's table id -> {use = last frame's {cols, rows}, new = this frame's}

        local function table_star(width, level)
            if width == nil or width == '*' then return 1 end
            local s = tostring(width)
            local n = tonumber(s:match('^(%d*%.?%d*)%*$') or '')
            if not n or n <= 0 then
                error('column width is a number, "auto", "*" or "N*" (a share of the remaining width)', level + 1)
            end
            return n
        end

        -- {x, w} of every column for a table w wide (nil: as wide as its fixed and auto columns, the shares at their
        -- minimum): fixed widths first, 'auto' ones from the measures of the last frame, then the rest shared out by the
        -- stars; a column that would pass its min / max is pinned there and the others share what is left.
        local function table_columns(columns, w, gap, auto, level)
            local n = #columns
            if n == 0 then error('a table needs columns', level + 1) end
            local specs, used = {}, gap * (n - 1)
            for i, c in ipairs(columns) do
                if type(c) ~= 'table' then c = {width = c} end
                local s = {c = c}
                if type(c.width) == 'number' then
                    s.w = c.width
                elseif c.width == 'auto' then
                    s.w = math.max(c.min or 0, math.min(c.max or math.huge, (auto and auto[i]) or 0))
                else
                    s.star = table_star(c.width, level + 1)
                end
                if s.w then used = used + s.w elseif c.min then used = used + c.min end
                specs[i] = s
            end
            if not w then w = used end
            local free = w - gap * (n - 1)
            for _, s in ipairs(specs) do if s.w then free = free - s.w end end
            local open = {}
            for _, s in ipairs(specs) do if s.star then open[#open + 1] = s end end
            while #open > 0 do
                local total = 0
                for _, s in ipairs(open) do total = total + s.star end
                local pinned, rest = false, {}
                for _, s in ipairs(open) do
                    local v = math.max(0, free) * s.star / total
                    local lo, hi = s.c.min, s.c.max
                    if lo and v < lo then s.w, pinned = lo, true; free = free - lo
                    elseif hi and v > hi then s.w, pinned = hi, true; free = free - hi
                    else rest[#rest + 1] = s end
                end
                if not pinned then
                    for _, s in ipairs(rest) do s.w = math.max(0, free) * s.star / total end
                    break
                end
                open = rest
            end
            local cols, cx = {}, 0
            for i, s in ipairs(specs) do
                -- Whole units: the edges are rounded, so neighbours never overlap or leave a hairline.
                local x0, x1 = math.floor(cx + 0.5), math.floor(cx + s.w + 0.5)
                cols[i] = {x = x0, w = x1 - x0, align = s.c.align, valign = s.c.valign, title = s.c.title}
                cx = cx + s.w + gap
            end
            return cols, w
        end

        function ui.table(id, x, y, w, opts)
            opts = opts or {}
            local gap = opts.spacing
            local gx, gy
            if type(gap) == 'table' then gx, gy = gap[1] or 8, gap[2] or 4 else gx, gy = gap or 8, gap or 4 end
            if w == 'auto' then w = nil end
            local key = tostring(id)
            local st = table_auto[key] or {}
            table_auto[key] = st
            st.use, st.new = st.new, {cols = {}, rows = {}}
            local use = st.use or {cols = {}, rows = {}}
            local b = opts.borders
            if b == true then b = {rows = true, columns = true, outer = true}
            elseif type(b) == 'table' then
                b = {rows = b.rows, columns = b.columns, outer = b.outer, color = b.color}
            else b = nil end
            local cols, tw = table_columns(opts.columns or {}, w, gx, use.cols, 2)
            local t = setmetatable({id = key, x = x, y = y, w = tw, gx = gx, gy = gy, opts = opts, borders = b,
                row_h = opts.row_height or 22, next_y = y, row_n = 0, body_n = 0, cy = y, ch = 0, cols = cols,
                state = st, use = use}, Table)
            local titled = false
            for _, c in ipairs(t.cols) do if c.title then titled = true end end
            if titled and opts.header ~= false then
                t.row_n = 1
                t.cy, t.ch = y, opts.header_height or t.row_h
                t:draw_borders(true, {})
                for i, c in ipairs(t.cols) do
                    if c.title then
                        t:label(i, c.title, {bold = true, color = C.accent, id = t.id .. '#head' .. i})
                    end
                end
                ui.rect(x, t.cy + t.ch + math.floor(gy / 2), tw, 1, C.edge_dim, {layer = opts.layer})
                t.next_y = t.cy + t.ch + gy + 1
                t.header = true
            end
            return t
        end

        -- The grid lines of the row just started (opts.borders): the column lines through the row and the gaps, a line
        -- above it (rows), the frame's sides and top (outer; the bottom is drawn by t:finish()).
        function Table:draw_borders(first, ropts)
            local b = self.borders
            if not b then return end
            local o = {layer = self.opts.layer}
            local color = b.color or C.edge_dim
            local pad = math.floor(self.gy / 2)
            local top, h = self.cy - pad, self.ch + 2 * pad
            if b.columns and ropts.columns ~= false then
                for c = 2, #self.cols do
                    ui.rect(self.x + self.cols[c].x - math.floor(self.gx / 2) - 1, top, 1, h, color, o)
                end
            end
            if b.outer then
                ui.rect(self.x, top, 1, h, color, o)
                ui.rect(self.x + self.w - 1, top, 1, h, color, o)
                if first then ui.rect(self.x, top, self.w, 1, color, o) end
            end
            if b.rows and not first and not (self.header and self.body_n == 1) then
                ui.rect(self.x, top, self.w, 1, color, o)
            end
        end

        -- t:row(height?, background?, opts?) -> row number (of the body, 1-based): the next row. `height`: a number
        -- (default the table's row_height), or 'auto' for the tallest cell of this row's last frame. `background`: an
        -- optional colour (opts.stripes shades every second row by itself). opts.columns = false: no column lines through
        -- this row (under a cell that spans columns).
        function Table:row(height, background, ropts)
            ropts = ropts or {}
            if height == 'auto' then
                height = self.use.rows[self.row_n + 1] or self.row_h
            end
            self.cy = self.next_y
            self.ch = height or (self.opts.row_heights and self.opts.row_heights[self.body_n + 1]) or self.row_h
            self.next_y = self.cy + self.ch + self.gy
            self.row_n, self.body_n = self.row_n + 1, self.body_n + 1
            local fill = background
            if not fill and self.opts.stripes and self.body_n % 2 == 0 then
                fill = self.opts.stripes == true and 0x30FFFFFF or self.opts.stripes
            end
            if fill then
                local pad = math.floor(self.gy / 2)
                ui.rect(self.x, self.cy - pad, self.w, self.ch + 2 * pad, fill, {layer = self.opts.layer})
            end
            self:draw_borders(not self.header and self.body_n == 1, ropts)
            return self.body_n
        end

        -- t:finish() -> height. Draws what needs the end (the frame's bottom line with borders.outer) and returns the
        -- table's height; the measures for 'auto' are kept as they are.
        function Table:finish()
            local b = self.borders
            if b and b.outer and self.next_y > self.y then
                local pad = math.floor(self.gy / 2)
                ui.rect(self.x, self.cy + self.ch + pad, self.w, 1, b.color or C.edge_dim, {layer = self.opts.layer})
            end
            return self:height()
        end

        -- t:column(col) -> x, w of the column (canvas coordinates).
        function Table:column(col)
            local c = self.cols[col]
            if not c then error('no column ' .. tostring(col) .. ' (the table has ' .. #self.cols .. ')', 3) end
            return self.x + c.x, c.w
        end

        -- t:cell(col, span?, rowspan?) -> x, y, w, h of the column (`span` columns from it, with the gaps between) in this
        -- row; with `rowspan` the cell reaches down over that many rows: the later rows' heights are not known yet, so they
        -- count as opts.row_heights[n] or row_height (give row_heights when the rows differ), and the cells of those rows in
        -- these columns are left to you (leave them out).
        function Table:cell(col, span, rowspan)
            local x0 = self:column(col)
            local last = col + (span or 1) - 1
            if last > #self.cols then error('the span passes the last column', 2) end
            local x1, w1 = self:column(last)
            local h = self.ch
            for k = 1, (rowspan or 1) - 1 do
                local rh = self.opts.row_heights and self.opts.row_heights[self.body_n + k] or self.row_h
                h = h + self.gy + rh
            end
            return x0, self.cy, x1 + w1 - x0, h
        end

        -- t:height() -> the height of everything so far (the header and the rows, without a trailing gap); the content
        -- height for scroll_end. t:width() is the table's width (its columns' sum when it was made without one).
        function Table:height()
            if self.next_y == self.y then return 0 end
            return self.next_y - self.gy - self.y
        end
        function Table:width() return self.w end

        -- Notes the natural size of a cell for the next frame's 'auto' columns and rows (a cell that spans does not count).
        function Table:measure(col, o, nw, nh)
            if (o.span or 1) == 1 and nw then
                local cur = self.state.new.cols[col]
                if not cur or nw > cur then self.state.new.cols[col] = nw end
            end
            if (o.rowspan or 1) == 1 and nh then
                local cur = self.state.new.rows[self.row_n]
                if not cur or nh > cur then self.state.new.rows[self.row_n] = nh end
            end
        end

        -- The place of a widget in its cell. Horizontally opts.halign (alias align; else the column's align): 'left',
        -- 'center', 'right', or 'stretch' (the cell's width); vertically opts.valign (else the column's): 'top', 'center'
        -- ('middle'), 'bottom' or 'stretch'. The default is stretch for what has no size of its own (buttons, dropdowns) unless
        -- opts.width / opts.height are given, else left and centred. opts.width / height / `nw` / `nh` set the size otherwise.
        function Table:place(col, o, nw, nh)
            local x, y, w, h = self:cell(col, o.span, o.rowspan)
            local c = self.cols[col]
            local ww, hh = o.width or nw, o.height or nh
            local ha = o.halign or o.align or (ww and (c.align or 'left') or 'stretch')
            local va = o.valign or c.valign or (hh and 'center' or 'stretch')
            if va == 'middle' then va = 'center' end
            if ha == 'stretch' or not ww then ww = w else ww = math.min(w, ww) end
            if va == 'stretch' or not hh then hh = h end
            local px, py = x, y
            if ha == 'right' then px = x + w - ww elseif ha == 'center' then px = x + math.floor((w - ww) / 2) end
            if va == 'bottom' then py = y + h - hh elseif va == 'center' then py = y + math.floor((h - hh) / 2) end
            return px, py, ww, hh
        end

        -- A copy of the widget's options with the table's layer and an automatic id.
        function Table:wopts(col, opts)
            local o = {}
            for k, v in pairs(opts or {}) do o[k] = v end
            o.layer = o.layer or self.opts.layer
            o.id = o.id or (self.id .. '#' .. self.row_n .. ',' .. col)
            return o
        end

        -- Text cut to `w` with "..." (cached).
        local function table_fit(text, w, mo)
            local tw = ui_measure(text, mo)
            if not tw or tw <= w then return text, tw end
            local key = table.concat({text, w, mo.size or 0, mo.face or '', mo.bold and 1 or 0, mo.italic and 1 or 0}, '\1')
            local hit = table_fit_cache[key]
            if hit then return hit[1], hit[2] end
            local lo, hi = 0, #text
            while lo < hi do
                local mid = math.ceil((lo + hi) / 2)
                -- never cut inside a UTF-8 sequence
                while mid > 0 and (text:byte(mid + 1) or 0) >= 0x80 and (text:byte(mid + 1) or 0) < 0xC0 do mid = mid - 1 end
                if mid <= lo then break end
                local mw = ui_measure(text:sub(1, mid) .. TABLE_ELLIPSIS, mo)
                if mw and mw <= w then lo = mid else hi = mid - 1 end
            end
            local cut = text:sub(1, lo) .. TABLE_ELLIPSIS
            local cw = ui_measure(cut, mo)
            if table_fit_n > 512 then table_fit_cache, table_fit_n = {}, 0 end
            table_fit_cache[key], table_fit_n = {cut, cw}, table_fit_n + 1
            return cut, cw
        end

        -- t:label(col, text, opts) -> drawn. opts: the label's (font, size, bold, color, tooltip, ...), halign (left, center,
        -- right; stretch is left), valign (top, center, bottom; default centre), span, rowspan, ellipsis (default true: too
        -- long a text is cut with "..." and shows whole as the tooltip).
        function Table:label(col, text, opts)
            local o = self:wopts(col, opts)
            local x, y, w, h = self:cell(col, o.span, o.rowspan)
            text = tostring(text)
            local full, fh = ui_measure(text, o)
            self:measure(col, o, full and math.ceil(full), fh and math.ceil(fh) + 4)
            local shown, tw = text, full
            if o.ellipsis ~= false then shown, tw = table_fit(text, w, o) end
            local ha = o.halign or o.align or self.cols[col].align or 'left'
            local px = x
            if tw and ha == 'right' then px = x + w - tw elseif tw and ha == 'center' then px = x + math.floor((w - tw) / 2) end
            local va = o.valign or self.cols[col].valign or 'center'
            o.valign = (va == 'stretch' or va == 'center') and 'middle' or va
            o.height = o.height or h
            if o.tooltip == nil and shown ~= text then o.tooltip = text end
            return ui.label(o.id, shown, px, y, o)
        end

        -- t:button(col, text, opts) -> clicked. Fills the cell unless opts.width / height / halign / valign say otherwise.
        function Table:button(col, text, opts)
            local o = self:wopts(col, opts)
            local tw, th = ui_measure(tostring(text), {})
            self:measure(col, o, (tw and math.ceil(tw) or 0) + 16 + (o.image and 18 or 0), th and math.ceil(th) + 8)
            local x, y, w, h = self:place(col, o)
            return ui.button(o.id, x, y, w, h, text, o)
        end

        -- t:checkbox(col, text, checked, opts) -> changed, checked
        function Table:checkbox(col, text, checked, opts)
            local o = self:wopts(col, opts)
            local tw = ui_measure(tostring(text), {}) or 60
            self:measure(col, o, 20 + math.ceil(tw), 16)
            local x, y = self:place(col, o, 20 + tw, 16)
            return ui.checkbox(o.id, x, y, text, checked, o)
        end

        -- t:dropdown(col, options, index, opts) -> changed, index. As high as the row (opts.height otherwise).
        function Table:dropdown(col, options, index, opts)
            local o = self:wopts(col, opts)
            local widest = 0
            for _, text in ipairs(options) do widest = math.max(widest, ui_measure(tostring(text), {}) or 0) end
            self:measure(col, o, math.ceil(widest) + 26, o.height or 18)
            local x, y, w, h = self:place(col, o)
            o.height = h
            return ui.dropdown(o.id, x, y, w, options, index, o)
        end

        -- t:slider(col, min, max, value, opts) -> changed, value. The number (unless opts.format == false) takes
        -- opts.value_width (default 44) at the cell's right end; with opts.width the slider is that wide and the number lies beside it.
        function Table:slider(col, min, max, value, opts)
            local o = self:wopts(col, opts)
            self:measure(col, o, nil, 16)
            local x, y, w = self:place(col, o, nil, 16)
            if o.format ~= false and not o.width then w = math.max(10, w - (o.value_width or 44)) end
            return ui.slider(o.id, x, y, w, min, max, value, o)
        end

        -- t:image(col, image, opts) -> drawn. opts: ui.image's (fit defaults to 'contain'), width, height, halign, valign.
        function Table:image(col, image, opts)
            local o = self:wopts(col, opts)
            self:measure(col, o, o.width, o.height)
            local x, y, w, h = self:place(col, o)
            o.fit = o.fit or 'contain'
            return ui.image(image, x, y, w, h, o)
        end
    end

    -- ui.panel(id, title, x, y, w, h, opts{closable, draggable, layer, bare}) -> panel {x, y, w, h, open, content_x,
    -- content_y}. A window of the mod's: the title bar drags it (it keeps the position for the session), its X
    -- closes it (panel.open false; ui.show_panel(id) opens it again). Its body takes the mouse, so clicks on it do
    -- not reach the game.
    -- Panels: a mod's windows. The layout keeps each one's place (and size, if resizable) under "mod_id:id".
    local panels = {}
    -- The open / closed state is kept in the layout (mods\gear_layout.json, "open": true / false) and comes back at the next
    -- start, after the mod's main.lua ran (its own show_panel calls there are the defaults). ui.persist_panel(id, false)
    -- takes a panel out (for screens that only make sense with a state of the mod: a dialog, a keyboard).
    local function remember_open(id, open)
        local p = panels[id]
        if mod.loading or (p and p.nopersist) then return end
        local key = mod.id .. ':' .. tostring(id)
        local lay = layout_get()
        local e = lay[key]
        if not e then e = {}; lay[key] = e end
        if e.open ~= open then e.open = open; layout_dirty = true end
    end
    function ui.show_panel(id, open)
        local p = panels[id] or {}
        panels[id] = p
        p.closed = open == false
        remember_open(id, not p.closed)
    end
    function ui.persist_panel(id, on)
        local p = panels[id] or {}
        panels[id] = p
        p.nopersist = on == false or nil
        if on == false then
            local e = layout_get()[mod.id .. ':' .. tostring(id)]
            if e and e.open ~= nil then
                e.open = nil
                if next(e) == nil then layout_get()[mod.id .. ':' .. tostring(id)] = nil end
                layout_dirty = true
            end
        end
    end
    -- Called by the kernel after main.lua: the kept states win over the defaults the mod set.
    function mod.restore_panels()
        local prefix = mod.id .. ':'
        for key, e in pairs(layout_get()) do
            if key:sub(1, #prefix) == prefix and type(e) == 'table' and e.open ~= nil then
                local id = key:sub(#prefix + 1)
                local p = panels[id] or {}
                panels[id] = p
                if not p.nopersist then p.closed = e.open == false end
            end
        end
    end
    function ui.panel_open(id)
        return not (panels[id] and panels[id].closed)
    end
    -- ui.toggle_panel(id) -> open: shows a hidden panel, hides a shown one.
    function ui.toggle_panel(id)
        local open = not ui.panel_open(id)
        ui.show_panel(id, open)
        return open
    end
    function ui.panel(id, title, x, y, w, h, opts)
        opts = opts or {}
        local p = panels[id] or {}
        panels[id] = p
        if p.closed then return {open = false} end
        local key = mod.id .. ':' .. tostring(id)
        local saved = layout_get()[key]
        if saved and not saved.x then saved = nil end
        if saved then
            x, y = from_virtual(saved.x, saved.y)
            if opts.resizable and saved.w then w, h = size_from_virtual(saved.w, saved.h) end
        end
        if p.x then x, y = p.x, p.y end         -- being dragged
        if p.w then w, h = p.w, p.h end         -- being resized
        local min_w, min_h = opts.min_width or 120, opts.min_height or 60
        w, h = math.max(w, min_w), math.max(h, min_h)
        if saved and not p.x then
            -- A kept place (a fraction of the screen, see from_virtual) or a size from another screen shape may not fit
            -- this one: the panel stays on the screen (left / top win when it is larger than the screen).
            local s = ui_tick_state or {}
            local sl, st_, sr, sb = s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768
            x = math.max(sl, math.min(x, sr - w))
            y = math.max(st_, math.min(y, sb - h))
        end
        local o = {layer = opts.layer}
        if opts.bare then
            -- No window: nothing drawn (the layout editor's mode shows the frame), no region (the mouse and clicks go on to the
            -- game), the mod draws in the whole rectangle. The layout editor still moves and, with opts.resizable, sizes it.
            if ui_edit then ui.frame(x, y, w, h, C.accent, 1, o) end
            ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = id, title = tostring(title), x = x, y = y,
                w = w, h = h, closable = false, back = false, resizable = opts.resizable and true or false,
                min_width = min_w, min_height = min_h, layer = opts.layer or 'hud'}
            return {open = true, key = key, x = x, y = y, w = w, h = h, content_x = x, content_y = y, content_w = w, content_h = h}
        end
        ui.rect(x, y, w, h, opts.background or 0xD80E0E14, o)
        ui.frame(x, y, w, h, ui_edit and C.accent or C.edge, ui_edit and 2 or 1, o)
        ui.rect(x + 1, y + 1, w - 2, 20, C.title, o)
        ui.text(title, x + 8, y + 1, {height = 20, valign = 'middle', color = C.accent, layer = opts.layer})
        region(tostring(id) .. '#body', x, y, w, h, {layer = opts.layer, nav = false})
        local function store()
            local vx, vy = to_virtual(x, y)
            local e = {x = vx, y = vy}
            if opts.resizable then e.w, e.h = size_to_virtual(w, h) end
            layout_set(key, e)
        end
        if opts.draggable ~= false then
            local bar = region(tostring(id) .. '#bar', x, y, w - 24, 21, {layer = opts.layer, tooltip = opts.tooltip, nav = false})
            if bar.pressed then p.grab_x, p.grab_y = ui_mouse.x - x, ui_mouse.y - y end
            if bar.held and p.grab_x then
                -- kept on the screen while it is dragged (left / top win when it is larger than the screen)
                local sl, st_, sr, sb = screen_rect()
                p.x = math.max(sl, math.min(ui_mouse.x - p.grab_x, sr - w))
                p.y = math.max(st_, math.min(ui_mouse.y - p.grab_y, sb - h))
                x, y = p.x, p.y
            elseif p.x then
                store()
                p.x, p.y, p.grab_x = nil, nil, nil
            end
        end
        if opts.resizable then
            local grip = region(tostring(id) .. '#grip', x + w - 14, y + h - 14, 14, 14, {layer = opts.layer, tooltip = 'Resize', nav = ui_edit})
            for i = 0, 2 do
                ui.rect(x + w - 4 - i * 4, y + h - 4, 2, 2, grip.hover and C.accent or C.edge, o)
                ui.rect(x + w - 4, y + h - 4 - i * 4, 2, 2, grip.hover and C.accent or C.edge, o)
            end
            if grip.pressed then p.grab_w, p.grab_h = ui_mouse.x - w, ui_mouse.y - h end
            if grip.held and p.grab_w then
                p.w, p.h = math.max(min_w, ui_mouse.x - p.grab_w), math.max(min_h, ui_mouse.y - p.grab_h)
            elseif p.w then
                w, h = p.w, p.h
                store()
                p.w, p.h, p.grab_w = nil, nil, nil
            end
        end
        if opts.closable ~= false then
            local cr = region(tostring(id) .. '#close', x + w - 21, y + 2, 18, 18, {layer = opts.layer, tooltip = 'Close'})
            ui.text('x', x + w - 21, y + 2, {width = 18, height = 18, align = 'center', valign = 'middle',
                color = cr.hover and 0xFFFF8080 or C.text, layer = opts.layer})
            if cr.clicked then p.closed = true; remember_open(id, false) end
            -- The pad's B closes the topmost closable panel (opts.back == false: the mod handles B itself).
            if opts.back ~= false and not opts.passive then
                ui_window_open = true
                if ui_pad.back > 0 then
                    local top = nil
                    for _, q in ipairs(ui_panel_prev) do
                        if q.closable and q.back then top = q.key end
                    end
                    if top == key or (top == nil) then
                        p.closed = true
                        remember_open(id, false)
                        ui_pad.back = ui_pad.back - 1
                    end
                end
            end
        end
        ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = id, title = tostring(title), x = x, y = y,
            w = w, h = h, closable = opts.closable ~= false, back = opts.back ~= false and not opts.passive,
            resizable = opts.resizable and true or false, min_width = min_w, min_height = min_h,
            layer = opts.layer or 'hud'}
        return {open = true, key = key, x = x, y = y, w = w, h = h, content_x = x + 8, content_y = y + 28,
            content_w = w - 16, content_h = h - 36}
    end

    -- Layout editing (like an "edit mode" in other games): the panels of every mod show their ids and their resize
    -- grips while it is on; a layout editor (the layout mod of sacred-gear-mods) moves the game's windows too.
    -- ui.nav_modal(on): the mod's screen takes the controller alone while on: the pad's D-pad only reaches its
    -- widgets (and nothing else), B is ui.back_pressed(), and the mod menu button is not offered.
    function ui.nav_modal(on)
        if on then ui_nav_modal = mod elseif ui_nav_modal == mod then ui_nav_modal = nil end
    end
    -- ui.replace_window(name, on): the mod draws its own screen in place of the game's window `name` (in game a
    -- control name such as UI_WND_SAVEGAME, in the menus UI_MENU_LOADGAME ...): while it is open, the game's window
    -- is not drawn and gets no mouse or keys. -> true | false, message. Each tick it shows its screen the mod calls
    -- ui.replaced(name); without that the game's window comes back (1.5 s after it opened, 0.5 s after the last).
    function ui.replace_window(name, on)
        name = tostring(name):upper()
        if not native.ui_replace_known(name) then return false, 'no window ' .. name end
        local owner = uis.replace[name]
        if on == false then
            if owner == mod then uis.replace[name] = nil end
            return true
        end
        if owner and owner ~= mod and not owner.disabled and not owner.unloaded then
            return false, name .. ' is replaced by ' .. tostring(owner.id)
        end
        uis.replace[name] = mod
        return true
    end
    -- ui.replaced(name) -> bool: the game's window is open and the mod's screen takes its place now (call it every
    -- tick the screen is drawn; false: draw nothing, the window is closed or the game's own shows).
    function ui.replaced(name)
        name = tostring(name):upper()
        if uis.replace[name] ~= mod then return false end
        return native.ui_replace_beat(name)
    end
    -- ui.window_open(name) -> bool: whether the game's window (or menu screen) is open.
    function ui.window_open(name) return native.ui_window_open(tostring(name):upper()) end
    -- ui.close_window(name) -> bool: closes the game's window as its own Back (or Esc) does.
    function ui.close_window(name) return native.ui_close_window(tostring(name):upper()) end
    -- ui.menu_entries(name) -> {{text, x, y, w, h}, ...}: the entries of the game menu ('UI_WND_ESCMENU') in their order, with the game's own
    -- text (in its language) and rectangles; empty while the menu is closed. ui.menu_click(name, index) presses entry `index` (1-based)
    -- as the game's own click would, also while a mod replaces the menu (ui.replace_window).
    function ui.menu_entries(name)
        local list = native.ui_menu_entries and native.ui_menu_entries(tostring(name):upper()) or {}
        -- The labels are the game's texts by id (the control keeps the id, not the text).
        for _, e in ipairs(list) do
            if (e.text == nil or e.text == '') and e.text_id and e.text_id > 0 then e.text = native.text_get(e.text_id) or '' end
        end
        return list
    end
    function ui.menu_click(name, index) return native.ui_menu_click and native.ui_menu_click(tostring(name):upper(), index) or false end
    -- ui.back_pressed() -> bool: the controller's B was pressed (consumed: true once per press). Mods with a
    -- nav_modal screen or a panel with `back = false` close or step back with it.
    function ui.back_pressed()
        if ui_pad.back > 0 then
            ui_pad.back = ui_pad.back - 1
            return true
        end
        return false
    end
    -- ui.page_pressed() -> -1, 0 or 1: the controller's LB (-1) or RB (+1) was pressed while a nav_modal screen or a window of a mod is open (consumed;
    -- both in one tick cancel out). The previous / next page, tab or step of that screen. Show the prompts (<span class="padkey lb"></span> / rb) by it.
    function ui.page_pressed()
        local page = ui_pad.page
        if page == 0 then return 0 end
        ui_pad.page = 0
        return page > 0 and 1 or -1
    end
    -- ui.select_pressed() -> bool: the controller's Select / "-" button was pressed (consumed: true once per press, whoever asks first; ask only in the
    -- state where it means something to the mod, e.g. while the escape menu is open).
    function ui.select_pressed()
        if ui_pad.select > 0 then
            ui_pad.select = ui_pad.select - 1
            return true
        end
        return false
    end
    -- ui.menu_item(id, label, fn): an entry of the controller's mod menu (the pad button reaches it where a mod's
    -- tool needs a hotkey, ALT or a click on a tiny button). Declared keys (mod.lua) are listed there on their own.
    function ui.menu_item(id, label, fn)
        assert(type(fn) == 'function', 'menu_item wants a function')
        local items = ui_menu_items[mod] or {}
        ui_menu_items[mod] = items
        for _, it in ipairs(items) do
            if it.id == id then it.label, it.fn = tostring(label), fn return end
        end
        items[#items + 1] = {id = id, label = tostring(label), fn = fn}
    end
    -- Drag and drop between mods. A source is a widget or region the mod already draws; targets are other widgets or regions, of
    -- any mod, and the world. See src/gear/meta/ui.lua.
    -- ui.drag_source(id, result, {type, data, label, icon, icon_size, threshold, carry, on_end}): call after the region `id` (its result
    -- `result`): a press that moves `threshold` units (default 6) starts a drag of `type` carrying `data`. With the controller, a click
    -- picks the thing up (carry) and a click on a target puts it down (B cancels). -> true while this source is being dragged.
    function ui.drag_source(id, r, opts)
        assert(type(opts) == 'table' and type(opts.type) == 'string', 'drag_source(id, region, {type = "...", data = ...})')
        local handle = ui_handle(mod, id)
        local d = uis.drag
        if d then return d.handle == handle and d.mod == mod end
        if r.hidden or opts.enabled == false then return false end
        if r.pressed then uis.press_h, uis.press_x, uis.press_y = handle, ui_mouse.x, ui_mouse.y end
        if r.held and uis.press_h == handle then
            local dx, dy = ui_mouse.x - uis.press_x, ui_mouse.y - uis.press_y
            local th = opts.threshold or uis.DRAG_THRESHOLD
            if dx * dx + dy * dy >= th * th then
                uis.press_h = nil
                uis.drag_start(mod, id, handle, opts, false)
                return true
            end
        elseif r.clicked and opts.carry ~= false and native.ui_pad_info and native.ui_pad_info() then
            uis.drag_start(mod, id, handle, opts, true)
            return true
        end
        return false
    end
    -- ui.drop_target(id, {accept, hint, on_drop}) -> {dragging, over, valid, reason}: call after the region `id` every tick. While
    -- nothing is dragged it costs nothing and returns a constant table. `accept` is a type, a list of types, '*', or function(drag) -> ok, text.
    function ui.drop_target(id, opts)
        local d = uis.drag
        if not d then return uis.NOT_DRAGGING end
        assert(type(opts) == 'table' and type(opts.on_drop) == 'function', 'drop_target(id, {accept = ..., on_drop = function(drag, info) end})')
        local handle = ui_handle(mod, id)
        local found
        for i = ui_regions.n, 1, -1 do
            if ui_regions[i].handle == handle then found = ui_regions[i]; break end
        end
        if not found then return {dragging = true, over = false, valid = false} end
        local n = uis.drop_targets.n + 1
        uis.drop_targets.n = n
        local t = uis.drop_targets[n]
        if not t then t = {}; uis.drop_targets[n] = t end
        t.handle, t.mod, t.opts, t.id = handle, mod, opts, id
        local ok, text = uis.drag_accepts(opts.accept, d)
        if ok and d.handle ~= handle and d.carry then
            for i = ui_nav.n, 1, -1 do       -- with the pad's pick-up, the valid targets are the only stops
                local e = ui_nav[i]
                if e.mod == mod and e.x == found.x and e.y == found.y and e.w == found.w and e.h == found.h then e.drop = true; break end
            end
        end
        return {dragging = true, over = ui_hot == handle and d.handle ~= handle, valid = ok and d.handle ~= handle or false, reason = text or (ok and opts.hint or nil)}
    end
    -- ui.world_drop(id, {accept, hint, on_drop}): the world takes drops of what `accept` allows (as for drop_target) while the cursor
    -- is on it; on_drop(drag, spot) gets spot = {x, y, layer, object_id}, the world point under the cursor. A second call with the same id replaces it, `ui.world_drop(id, nil)` removes it.
    function ui.world_drop(id, opts)
        local list = uis.world_drops
        for i = #list, 1, -1 do
            if list[i].mod == mod and list[i].id == id then table.remove(list, i) end
        end
        if opts == nil or opts == false then return end
        assert(type(opts) == 'table' and type(opts.on_drop) == 'function', 'world_drop(id, {accept = ..., on_drop = function(drag, spot) end})')
        list[#list + 1] = {mod = mod, id = id, opts = opts}
    end
    -- ui.drag_active() -> the drag now on ({type, data, label, source, ...}) or nil; ui.drag_cancel() ends one this mod's source started.
    function ui.drag_active() return uis.drag end
    function ui.drag_cancel()
        if uis.drag and uis.drag.mod == mod then uis.drag_end(false) end
    end
    function ui.edit_mode() return ui_edit end
    function ui.set_edit_mode(on) ui_edit = on and true or false end
    function ui.can_move_windows() return native.ui_can_move_windows() end
    -- The panels of every mod drawn this tick (those not drawn yet: as of the last tick), in draw order:
    -- {key, mod, id, title, x, y, w, h, resizable, min_width, min_height, layer}.
    local function find_panel(key)
        for _, list in ipairs({ui_panel_list, ui_panel_prev}) do
            for _, e in ipairs(list) do
                if e.key == key then return e end
            end
        end
    end
    function ui.panels()
        local out, seen = {}, {}
        for _, list in ipairs({ui_panel_prev, ui_panel_list}) do
            for _, e in ipairs(list) do
                if not seen[e.key] then
                    seen[e.key] = #out + 1
                    out[#out + 1] = copy(e)
                else
                    out[seen[e.key]] = copy(e)      -- this tick's place wins
                end
            end
        end
        return out
    end
    -- Puts panel `key` ("mod_id:panel_id", any mod's) at (x, y) of the canvas, and, if it is resizable, makes it w x h
    -- (w, h nil: its size stays); kept in the layout. The panel takes it the next time its mod draws it.
    function ui.set_panel_rect(key, x, y, w, h)
        local e = find_panel(key)
        if not e then return false end
        local vx, vy = to_virtual(x, y)
        local entry = {x = vx, y = vy}
        if e.resizable then
            w = math.max(w or e.w, e.min_width)
            h = math.max(h or e.h, e.min_height)
            entry.w, entry.h = size_to_virtual(w, h)
        end
        layout_set(key, entry)
        e.x, e.y = x, y
        if e.resizable then e.w, e.h = w, h end
        return true
    end
    -- Panel `key` back to the place (and size) its mod gives it.
    function ui.reset_panel(key)
        if layout_get()[key] then layout_set(key, nil) end
        return find_panel(key) ~= nil
    end
    -- ui.capture_keys(on): while on, the keyboard does not reach the game (key messages are dropped, the game's key
    -- queries see nothing held); this mod's own key_pressed / key_down still work. Released when the mod unloads.
    function ui.capture_keys(on)
        key_capture[mod] = on and true or nil
    end
    -- ui.typed() -> string: what the player typed since the last tick while this mod captures the keys (UTF-8, in
    -- the player's keyboard layout: umlauts, Shift, AltGr; no control characters: read Backspace and Enter with
    -- key_pressed). For a text field.
    function ui.typed()
        return key_capture[mod] and uis.typed or ''
    end
    -- ui.key_down(name): whether the key is held (while the game has the focus).
    function ui.key_down(name)
        local vk = type(name) == 'number' and name or UI_VK[tostring(name):lower()]
        if not vk then error('unknown key ' .. tostring(name), 2) end
        return native.key_down(vk)
    end
    -- The game window `name` (a control name) with its top-left corner at (x, y) of the canvas; kept in the layout.
    function ui.set_window_position(name, x, y)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() and w.movable then
                if not native.ui_move_window(w.slot, x - w.x, y - w.y, true) then return false end
                local vx, vy = to_virtual(x, y)
                layout_set('game:' .. w.name, {x = vx, y = vy})
                return true
            end
        end
        return false
    end
    -- Back to its own place (and out of the layout).
    function ui.reset_window(name)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() then
                native.ui_move_window(w.slot, 0, 0, false)
                if layout_get()['game:' .. w.name] then layout_set('game:' .. w.name, nil) end
                return true
            end
        end
        return false
    end
    -- Forgets the places of all panels and game windows (or the ones whose key starts with `prefix`).
    function ui.reset_layout(prefix)
        for key in pairs(layout_get()) do
            if not prefix or key:sub(1, #prefix) == prefix then
                if key:sub(1, 5) == 'game:' then ui.reset_window(key:sub(6)) else layout_set(key, nil) end
            end
        end
        for _, p in pairs(panels) do p.x, p.y, p.w, p.h = nil, nil, nil, nil end
    end
    -- The stored layout: {key = {x, y, w, h}} in the virtual 4096 x 4096 screen.
    function ui.layout()
        local out = {}
        for k, v in pairs(layout_get()) do
            if v.x then out[k] = {x = v.x, y = v.y, w = v.w, h = v.h, open = v.open} end
        end
        return out
    end
    function ui.to_virtual(x, y) return to_virtual(x, y) end
    function ui.from_virtual(vx, vy) return from_virtual(vx, vy) end

    -- ui.text_size(text, font_or_opts): a font (see ui.text) or a table of ui.text's font options.
    function ui.text_size(text, font)
        return ui_measure(tostring(text), type(font) == 'table' and font or {font = font})
    end
    function ui.windows() return native.ui_windows() end
    function ui.window(name)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() then return w end
        end
        return nil
    end
    function ui.state() return native.ui_state() end
    -- ui.tooltip_rect(w, h, opts) -> x, y: where to put a w x h tooltip (or any popup beside the cursor) so that it is
    -- clear of the game's cursor sprite and inside the screen. opts: x, y (the point to avoid, default the cursor),
    -- bounds {left, top, right, bottom} (default the screen), cursor = false (the point has no sprite: only the gap).
    function ui.tooltip_rect(w, h, opts)
        assert(type(w) == 'number' and type(h) == 'number', 'w and h must be numbers')
        opts = opts or {}
        assert(type(opts) == 'table', 'opts must be a table')
        local s = native.ui_state()
        local b = opts.bounds
        if b ~= nil then
            assert(type(b) == 'table' and #b == 4, 'bounds must be {left, top, right, bottom}')
        else
            b = {s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768}
        end
        local mx = opts.x or ui_mouse.x
        local my = opts.y or ui_mouse.y
        local cw, ch = UI_CURSOR_W, UI_CURSOR_H
        if opts.cursor == false then cw, ch = 0, 0 end
        return tooltip_place(w, h, mx, my, b, cw, ch, 4)
    end
    function ui.screen()
        local s = native.ui_state()
        return s.screen_left, s.screen_top, s.screen_right, s.screen_bottom
    end
    return ui
end

