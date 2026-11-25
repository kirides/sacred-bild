-- gear.toast: short messages that stack up on the screen and fade away (an unlocked achievement, a finished bounty). Shipped
-- inside Sacred Gear and run once per mod, in the mod's own environment (so `gear` below is the mod's). It only uses the
-- public API.
--
-- The stack is a bare panel of the mod ('toasts'): the layout editor moves it (it shows a sample there while the editor is on),
-- the mouse and the controller go on to the game. A toast's age counts in-game frames, so it does not run out behind a menu.

local ui = gear.ui
local T = {}

local PANEL = 'toasts'
local PAD, LINE = 12, 15              -- padding all around, line height of the text
local GAP = 8                         -- between two toasts

local opts = {seconds = 6, max = 4, width = 360}
local toasts = {}                     -- oldest first: {title, lines, born, color}
local now = 0

-- Options for this mod's toasts (each is kept until changed):
--   seconds  how long a toast stays (default 6; a number or a function returning one, e.g. a setting)
--   max      how many are shown at once (default 4; the oldest goes first)
--   width    the width of a toast in layout units (default 360)
--   when     a function(state) -> boolean: whether toasts are drawn now (default: on the game screen)
function T.options(o)
    for k, v in pairs(o or {}) do opts[k] = v end
end

-- Shows a toast: a title and any number of lines (nil and '' are left out). The title may be a table
-- {title = , lines = {...}, color = 0xAARRGGBB (the accent colour)}.
function T.show(title, ...)
    local t
    if type(title) == 'table' then
        t = {title = tostring(title.title or ''), lines = {}, color = title.color}
        for _, line in ipairs(title.lines or {}) do
            if line ~= nil and line ~= '' then t.lines[#t.lines + 1] = tostring(line) end
        end
    else
        t = {title = tostring(title == nil and '' or title), lines = {}}
        for i = 1, select('#', ...) do
            local line = select(i, ...)
            if line ~= nil and line ~= '' then t.lines[#t.lines + 1] = tostring(line) end
        end
    end
    t.born = now
    toasts[#toasts + 1] = t
    while #toasts > (tonumber(opts.max) or 4) do table.remove(toasts, 1) end
    return t
end

-- Removes every toast of this mod.
function T.clear() toasts = {} end

-- How many toasts are on screen.
function T.count() return #toasts end

local function life()
    local s = opts.seconds
    if type(s) == 'function' then s = s() end
    return math.max(0.5, tonumber(s) or 6)
end

local function scaled(color, a)
    local alpha = math.floor(((color or 0xFFFFFFFF) / 0x1000000) % 256 * a)
    return alpha * 0x1000000 + (color or 0xFFFFFFFF) % 0x1000000
end

-- The lines of a toast wrapped to the width (kept with the toast: wrapping is not free).
local function wrapped(t, tw)
    if t.wrap_w ~= tw then
        local lines = {}
        for _, line in ipairs(t.lines) do
            for _, part in ipairs((ui.text_wrap(line, tw - 2 * PAD - 4, {size = 12}))) do lines[#lines + 1] = part end
        end
        t.wrapped, t.wrap_w = lines, tw
    end
    return t.wrapped
end

local function height(t, tw)
    local n = #wrapped(t, tw)
    -- title row 18, a gap and a hairline when there is text, then the lines; PAD above and below
    return PAD + 18 + (n > 0 and (8 + n * LINE) or 0) + PAD
end

local function draw_one(t, x, y, tw, a)
    local lines = wrapped(t, tw)
    local th = height(t, tw)
    local accent = t.color or ui.rgba(224, 169, 68, 255)
    local o = {layer = 'top'}
    ui.rect(x, y, tw, th, ui.rgba(12, 10, 8, math.floor(215 * a)), o)
    ui.rect(x, y, 3, th, scaled(accent, a), o)
    ui.frame(x, y, tw, th, ui.rgba(200, 170, 90, math.floor(200 * a)), 1, o)
    ui.text(t.title, x + PAD + 4, y + PAD, {color = ui.rgba(240, 200, 110, math.floor(255 * a)), bold = true, size = 14, layer = 'top'})
    if #lines > 0 then
        ui.rect(x + PAD + 4, y + PAD + 21, tw - 2 * PAD - 8, 1, ui.rgba(200, 170, 90, math.floor(90 * a)), o)
        local ly = y + PAD + 28
        for _, line in ipairs(lines) do
            ui.text(line, x + PAD + 4, ly, {color = ui.rgba(255, 255, 255, math.floor(230 * a)), size = 12, layer = 'top'})
            ly = ly + LINE
        end
    end
    return th
end

local sample = {title = 'Toasts', lines = {}}

local function draw(state)
    local editing = ui.edit_mode and ui.edit_mode()
    local shown
    if opts.when then shown = opts.when(state) else shown = state.mode == 'game' end
    if not shown and not editing then return end
    local tw = tonumber(opts.width) or 360
    local span = life()
    local keep, total = {}, 0
    for _, t in ipairs(toasts) do
        if now - t.born < span then
            keep[#keep + 1] = t
            total = total + height(t, tw) + GAP
        end
    end
    toasts = keep
    if #toasts == 0 and not editing then return end
    if editing and #toasts == 0 then
        sample.title = ((gear.mod and gear.mod.name) or 'Mod') .. ': messages'
        sample.lines[1] = 'Move this box to place them.'
        sample.wrap_w = nil
        total = height(sample, tw) + GAP
    end
    local p = ui.panel(PANEL, ((gear.mod and gear.mod.name) or 'Mod') .. ': messages', 512 - tw / 2, 14, tw, math.max(60, total - GAP), {bare = true})
    if not p.open then return end
    local x, y = p.content_x or (512 - tw / 2), p.content_y or 14
    if #toasts == 0 then
        draw_one(sample, x, y, tw, 0.7)
        return
    end
    for _, t in ipairs(toasts) do
        local age = now - t.born
        local a = math.max(0, math.min(1, (span - age) / 0.6, age / 0.2 + 0.2))
        y = y + draw_one(t, x, y, tw, a) + GAP
    end
end

gear.on('frame', function(dt)
    dt = tonumber(dt) or 0
    if dt > 0 and dt < 5 then now = now + dt end
end)
gear.on('draw', gear.guard('gear.toast draw', function(state)
    if type(state) == 'table' then draw(state) end
end))

return T
