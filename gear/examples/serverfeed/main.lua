-- Server feed: on-screen announcements from the game server's scripts (SacredEngineRemake Sacred.GameServer,
-- scripts-examples/rare_drops.lua and graveyard.lua) over gear.net channels:
--   'rare_drops:feed'  {kind = 'drop', drop = {item, tier, rarity, by, from, time}}  /  {kind = 'recent', drops = {...}}
--   'graveyard:feed'   {kind = 'death', name, level, by, killer_kind, time}           /  {kind = 'recent', deaths = {...}}
-- On connecting, this mod sends an (empty) table on each channel: the server answers with the last five of each as 'recent', which
-- only fill the list (F7), without toasts. A server without those scripts (or without custom net messages) just never sends.
local ui, net, S = gear.ui, gear.net, gear.settings

local HISTORY = 12
local history = {}      -- newest first: {text, color}
local toasts = {}       -- {text, color, born}

-- the game's item label colours by rarity tier (docs/Gear.md "Pickup")
local function rarity_color(r)
    if (r or 0) >= 15 then return 0xFFC19A33 elseif (r or 0) >= 12 then return 0xFFF0E040 elseif (r or 0) >= 5 then return 0xFFA0A0FF end
    return 0xFFEFEFEF
end
local DEATH = 0xFFE07070

local function drop_text(d)
    local who = d.by and (tostring(d.by) .. ' found a ') or 'A '
    return who .. tostring(d.tier or 'rare') .. ' ' .. tostring(d.item or 'item') .. (d.from and (', dropped by ' .. tostring(d.from)) or '')
end

local function death_text(d)
    if d.killer_kind == 'unknown' or not d.by then return tostring(d.name) .. ' has died' end
    return tostring(d.name) .. ' (level ' .. tostring(d.level or 0) .. ') was slain by ' .. tostring(d.by)
end

local function remember(text, color)
    table.insert(history, 1, {text = text, color = color})
    while #history > HISTORY do table.remove(history) end
end

local function announce(text, color, live)
    remember(text, color)
    if live then
        toasts[#toasts + 1] = {text = text, color = color, born = gear.time()}
        while #toasts > 5 do table.remove(toasts, 1) end
    end
end

local drops = net.channel('rare_drops:feed')
local deaths = net.channel('graveyard:feed')

if drops then
    drops:on(function(from, p)
        if type(p) ~= 'table' then return end
        if p.kind == 'drop' and type(p.drop) == 'table' then
            announce(drop_text(p.drop), rarity_color(p.drop.rarity), S.get('drops'))
        elseif p.kind == 'recent' and type(p.drops) == 'table' then
            for i = #p.drops, 1, -1 do announce(drop_text(p.drops[i]), rarity_color(p.drops[i].rarity), false) end
        end
    end)
end
if deaths then
    deaths:on(function(from, p)
        if type(p) ~= 'table' then return end
        if p.kind == 'death' then
            announce(death_text(p), DEATH, S.get('deaths'))
        elseif p.kind == 'recent' and type(p.deaths) == 'table' then
            for i = #p.deaths, 1, -1 do announce(death_text(p.deaths[i]), DEATH, false) end
        end
    end)
end

-- Ask for the latest ones once the server says it understands custom messages (the handshake can finish before this mod
-- runs, so the draw handler checks too).
local asked = false
local function ask()
    if asked then return end
    asked = true
    if drops then drops:send({want = 'recent'}, 'server') end
    if deaths then deaths:send({want = 'recent'}, 'server') end
end
gear.on('net_caps', function(caps) if caps.supported then ask() end end)

local show_history = false
gear.keys.on('history', function() show_history = not show_history end)

local BACK = ui.rgba(0, 0, 0, 150)

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then return end
    if not asked and net.supported() then ask() end
    local right, top = S.get('x') or 1008, S.get('y') or 180
    local life = S.get('seconds') or 8
    local now = gear.time()
    local y = top
    for i = #toasts, 1, -1 do
        local t = toasts[i]
        if now - t.born > life then
            table.remove(toasts, i)
        else
           
            ui.text(t.text, right - 420, y, {width = 420, align = 'right', color = t.color, shadow = true, background = BACK, padding = 2})
            y = y + 20
        end
    end
    if show_history then
        y = y + 8
        ui.text('Latest from the server (F7 hides)', right - 420, y, {width = 420, align = 'right', color = ui.rgba(255, 255, 255, 200), background = BACK, padding = 2})
        for _, h in ipairs(history) do
            y = y + 18
            ui.text(h.text, right - 420, y, {width = 420, align = 'right', color = h.color, background = BACK, padding = 2})
        end
        if #history == 0 then
            ui.text('nothing yet', right - 420, y + 18, {width = 420, align = 'right', color = ui.rgba(255, 255, 255, 140), background = BACK, padding = 2})
        end
    end
end)
