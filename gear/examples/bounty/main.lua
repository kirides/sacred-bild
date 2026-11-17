-- Bounty board client: shows the bounties a game server posts (Sacred.GameServer, scripts-examples/bounty_board.lua) and the
-- hero's own progress. The server pushes the board over the custom net channel 'bounty:state' (gear.net, so the server must be the
-- Sacred Gear game server; the original GameServer.exe never answers and the panel says so).
--
--   F8 (rebindable in the Mods screen)   the bounty panel: every bounty with a progress bar, time left and reward
--   a small tracker with the open bounties (setting "Tracker")
--
-- Protocol: the server sends { v = 1, next_in, bounties = { { id, title, text, region, target, n, done, left, reward, claims, level } } }
-- to a client that said HELLO (the library does that after joining); this mod also asks with { req = 1 } now and then.
-- `left` is relative (seconds), so this computer's clock does not matter. Nothing in the payload is trusted for anything but display.

local cfg = gear.config
local ui, net = gear.ui, gear.net

local channel = net and net.channel('bounty:state')
local board = nil          -- the last payload
local received = 0         -- gear.time() when it arrived
local asked = -1e9

local GOLD, GREEN, GREY = 0xFFFFD780, 0xFF80FF80, 0xFFB0B0B0

local function valid(payload)
    if type(payload) ~= 'table' or payload.v ~= 1 or type(payload.bounties) ~= 'table' then return false end
    for _, b in ipairs(payload.bounties) do
        if type(b) ~= 'table' or type(b.title) ~= 'string' or type(b.target) ~= 'number' then return false end
    end
    return true
end

if channel then
    channel:on(function(from, payload)
        if valid(payload) then board, received = payload, gear.time() end
    end)
end

local function ask(force)
    if not channel or not net.supported() then return end
    local t = gear.time()
    if not force and t - asked < cfg.refresh then return end
    asked = t
    channel:send({req = 1}, 'server')
end

gear.on('net_caps', function(caps) if caps and caps.supported then ask(true) end end)
gear.on('session.start', function()
    board = nil
    ask(true)
    ui.show_panel('board', false)       -- closed until F8
end)
gear.on('session.stop', function() board = nil end)
gear.every(5, function() ask(false) end)

local function time_text(seconds)
    seconds = math.max(0, math.floor(seconds))
    if seconds >= 3600 then return ('%dh %02dm'):format(math.floor(seconds / 3600), math.floor(seconds % 3600 / 60)) end
    if seconds >= 60 then return ('%dm %02ds'):format(math.floor(seconds / 60), seconds % 60) end
    return seconds .. 's'
end

local function left_of(b) return (b.left or 0) - (gear.time() - received) end

-- One bounty at (x, y) in a width of w; returns its height.
local ROW = 58
local function row(b, x, y, w)
    local done = b.done == true
    local n = math.min(b.n or 0, b.target)
    ui.text(b.title, x, y, {color = done and GREEN or GOLD, size = 15, bold = true})
    ui.text(done and 'done' or time_text(left_of(b)), x, y, {width = w, align = 'right', color = done and GREEN or GREY})
    ui.text((b.text or '') .. (b.level and b.level > 1 and (' (level ' .. b.level .. '+)') or ''),
        x, y + 17, {color = 0xFFE0E0E0, size = 12})
    local by = y + 33
    ui.rect(x, by, w, 10, ui.rgba(0, 0, 0, 160))
    local frac = b.target > 0 and n / b.target or 0
    if frac > 0 then ui.rect(x, by, math.max(1, math.floor(w * frac)), 10, done and ui.rgba(80, 200, 100, 220) or ui.rgba(210, 160, 60, 220)) end
    ui.frame(x, by, w, 10, ui.rgba(200, 170, 90, 160), 1)
    ui.text(('%d / %d'):format(n, b.target), x, by - 2, {width = w, align = 'center', size = 11, shadow = true})
    ui.text('Reward: ' .. tostring(b.reward or '?'), x, by + 12, {color = GREY, size = 11})
    return ROW
end

local function panel()
    local p = ui.panel('board', 'Bounty board (F8)', 300, 70, 400, 320, {resizable = true, min_width = 300, min_height = 160})
    if not p.open then return end
    local x, y, w, h = p.content_x, p.content_y, p.content_w, p.content_h
    if not channel or not net.supported() then
        local lines = ui.text_wrap('This server has no bounty board (it needs the Sacred Gear game server with bounty_board.lua).', w, {size = 12})
        for i, line in ipairs(lines) do ui.text(line, x, y + (i - 1) * 15, {color = GREY, size = 12}) end
        return
    end
    if not board then
        ui.text('Waiting for the server...', x, y, {color = GREY})
        return
    end
    if #board.bounties == 0 then
        ui.text('The board is empty.', x, y, {color = GREY})
        return
    end
    local area = ui.scroll_begin('bounty-list', x, y, w, h - 4)
    local cy = area.content_y
    for _, b in ipairs(board.bounties) do
        row(b, area.content_x, cy, area.w - 4)
        cy = cy + ROW + 6
    end
    ui.scroll_end(cy - area.content_y)
end

local function tracker()
    if not gear.settings.get('tracker') or not board then return end
    local lines = {}
    for _, b in ipairs(board.bounties) do
        if not b.done then lines[#lines + 1] = b end
    end
    if #lines == 0 then return end
    local x, y = cfg.tracker_x, cfg.tracker_y
    ui.text('Bounties', x, y, {color = GOLD, size = 13, bold = true, shadow = true})
    for i, b in ipairs(lines) do
        ui.text(('%s  %d/%d'):format(b.title, math.min(b.n or 0, b.target), b.target), x, y + 4 + i * 14,
            {color = 0xFFE0E0E0, size = 12, shadow = true})
    end
end

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then return end
    tracker()
    panel()
end)

gear.keys.on('panel', function()
    ui.show_panel('board', not ui.panel_open('board'))
end)
