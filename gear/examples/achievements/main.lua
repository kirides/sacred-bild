-- Achievements and login streak, the client side of the game server's scripts-examples/achievements.lua.
--
-- The server (Sacred.GameServer with that script) pushes {op = 'unlock' | 'streak' | 'state'} on the custom net channel
-- 'achievements:sync'; this mod shows a toast for the first two and keeps the last 'state' for a panel (F7, rebind in the
-- Mods screen) with the streak, tomorrow's reward and every achievement with its progress. It asks for the state with
-- {op = 'get'} once the server's handshake is done and while the panel is open. Servers without the script, and the
-- original GameServer.exe, never answer: the panel then says so and nothing else happens.
local ui, net = gear.ui, gear.net

local GOLD = ui.rgba(240, 200, 110)
local DIM = ui.rgba(255, 255, 255, 140)
local DONE_BG = ui.rgba(74, 56, 20, 200)
local TODO_BG = ui.rgba(255, 255, 255, 22)

local ch = net.channel('achievements:sync')
local state = nil          -- the last {op = 'state'} table
local toasts = {}          -- {title, lines, born}
local now = 0              -- seconds, advanced by the frame event

local function request()
    if ch and net.supported() then ch:send({op = 'get'}, 'server') end
end

local function toast(title, ...)
    local lines = {}
    for i = 1, select('#', ...) do
        local line = select(i, ...)
        if line and line ~= '' then lines[#lines + 1] = line end
    end
    toasts[#toasts + 1] = {title = title, lines = lines, born = now}
    while #toasts > 4 do table.remove(toasts, 1) end
end

if ch then
    ch:on(function(from, p)
        if type(p) ~= 'table' then return end
        if p.op == 'state' then
            state = p
        elseif p.op == 'unlock' then
            toast('Achievement unlocked', tostring(p.name or '?'), tostring(p.desc or ''),
                p.reward and ('Reward: ' .. p.reward) or nil)
        elseif p.op == 'streak' then
            toast(('Daily login: day %d'):format(tonumber(p.streak) or 0), p.reward and ('Reward: ' .. p.reward) or nil,
                ('Best streak: %d'):format(tonumber(p.best) or 0))
        end
    end)
end

gear.on('net_caps', function(caps)
    if caps.supported then request() else state = nil end
end)
gear.on('load', function() request() end)

gear.on('frame', function(dt) now = now + (dt or 0) end)

-- keep the panel's numbers fresh while it is open
gear.every(30, function()
    if ui.panel_open('panel') then request() end
end)

gear.keys.on('panel', function()
    local open = not ui.panel_open('panel')
    ui.show_panel('panel', open)
    if open then request() end
end)

-- ---- drawing ----

local TW, TH = 340, 58
local function draw_toasts()
    local life = tonumber(gear.settings.get('seconds')) or 6
    local y = 14
    local keep = {}
    for _, t in ipairs(toasts) do
        local age = now - t.born
        if age < life then
            keep[#keep + 1] = t
            -- fades in over 0.2 s and out over the last 0.6 s
            local alpha = math.max(0, math.min(1, (life - age) / 0.6, age / 0.2 + 0.2))
            local x = 512 - TW / 2
            local th = 24 + 14 * #t.lines
            ui.rect(x, y, TW, th, ui.rgba(0, 0, 0, math.floor(190 * alpha)), {layer = 'top'})
            ui.frame(x, y, TW, th, ui.rgba(200, 170, 90, math.floor(230 * alpha)), 1, {layer = 'top'})
            ui.text(t.title, x + 10, y + 5, {color = ui.rgba(240, 200, 110, math.floor(255 * alpha)), bold = true, layer = 'top'})
            local ly = y + 24
            for _, line in ipairs(t.lines) do
                ui.text(line, x + 10, ly, {color = ui.rgba(255, 255, 255, math.floor(230 * alpha)), size = 12, layer = 'top'})
                ly = ly + 14
            end
            y = y + th + 6
        end
    end
    toasts = keep
end

local function bar(x, y, w, h, fraction)
    ui.rect(x, y, w, h, ui.rgba(255, 255, 255, 30))
    if fraction > 0 then ui.rect(x, y, math.max(1, w * fraction), h, ui.rgba(200, 160, 60, 220)) end
end

local ROW = 40
local function draw_panel()
    local px, py = gear.config.panel_x or 590, gear.config.panel_y or 40
    local p = ui.panel('panel', 'Achievements (F7)', px, py, 410, 480, {resizable = true, min_width = 330, min_height = 240})
    if not p.open then return end
    local x, y = p.content_x, p.content_y
    local w = p.x + p.w - 8 - x
    if not state then
        local msg = net.supported() and 'Waiting for the server...'
            or 'This server has no achievements.\nThey need the Sacred Gear game server with its achievements.lua script.'
        ui.text(msg, x, y + 4, {color = DIM})
        return
    end
    local list = state.ach or {}
    local streak, days = state.streak or 0, state.days or 0
    local done = 0
    for _, a in ipairs(list) do if a.done then done = done + 1 end end
    ui.text(('Login streak: %d day%s   best %d   %d day%s visited'):format(streak, streak == 1 and '' or 's',
        state.best or 0, days, days == 1 and '' or 's'), x, y, {color = GOLD})
    if state.next then
        ui.text((state.claimed and "Today's reward is claimed. Tomorrow: " or "Today's reward: ") .. state.next,
            x, y + 17, {color = DIM, size = 12})
    end
    ui.text(('%d of %d unlocked'):format(done, #list), x, y + 34, {color = ui.COLORS.accent})
    local top = y + 54
    local area = ui.scroll_begin('list', x, top, w, p.y + p.h - 8 - top, {step = ROW})
    local cy = area.content_y
    for i, a in ipairs(list) do
        local ry = cy + (i - 1) * ROW
        local aw = area.w - 6
        ui.rect(x, ry, aw, ROW - 4, a.done and DONE_BG or TODO_BG)
        ui.text(tostring(a.name), x + 6, ry + 2, {color = a.done and GOLD or ui.COLORS.text, bold = true})
        ui.text(tostring(a.desc), x + 6, ry + 20, {color = DIM, size = 11})
        if a.done then
            ui.text('Done', x, ry + 2, {width = aw - 6, align = 'right', color = GOLD})
        else
            local need = math.max(1, tonumber(a.need) or 1)
            local have = tonumber(a.have) or 0
            ui.text(('%d / %d'):format(math.floor(have), math.floor(need)), x, ry + 2,
                {width = aw - 6, align = 'right', color = DIM, size = 12})
            bar(x + aw - 86, ry + 22, 80, 5, math.min(1, have / need))
        end
    end
    ui.scroll_end(#list * ROW)
end

gear.on('draw', function(st)
    if not st or st.mode ~= 'game' then return end
    if gear.settings.get('toasts') then draw_toasts() end
    if not st.exclusive then draw_panel() end
end)
