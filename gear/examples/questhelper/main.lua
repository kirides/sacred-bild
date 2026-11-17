-- Quest helper: a panel (F8).
--   Quest items: read here. The quest items in your backpack and the quest items lying near your hero (distance in tiles, position).
--   Records: from the game server (SacredEngineRemake Sacred.GameServer, scripts-examples/quest_helper.lua, channel 'questhelper'): the quest
--   records of your hero with the names, areas and hints the server's owner wrote for them, which other heroes have the same record,
--   and a line on the screen when one is new or changed.
--
-- What this cannot do, and why: Gear has no access to the game's quest journal (active quests, their texts, map markers), and the server
-- cannot read it either: a hero file holds only "quest pairs" ([key][value] records) and the journal is state of the game's script
-- interpreter that no message carries. So there is no list of active or available quests here, and the keys of the pairs have names only
-- where the server owner has filled in a catalog. The panel says so instead of showing guesses.

local ui, net = gear.ui, gear.net
local ch = net.channel('questhelper')
local TABS = {'Quest items', 'Records', 'About'}
local DIM, OK, WARN, GOLD = ui.rgba(255, 255, 255, 150), ui.rgba(143, 206, 122), ui.rgba(255, 200, 120), ui.rgba(255, 215, 120)
local NEAR_TILES = 40

local S = {
    tab = 1, state = nil, reason = nil, asked = 0, opened = false, show_unknown = true, toast = nil, toast_until = 0,
    near = {}, near_at = 0, status = '',
}

local function fit(text, max)
    text = tostring(text == nil and '' or text):gsub('%c', ' ')
    if #text > max then text = text:sub(1, max - 1) .. '...' end
    return text
end

-- ---- the server ----
local function ask(watch)
    if not net.supported() then return false end
    local ok, err = ch:send({op = 'state', watch = watch and true or false}, 'server')
    if not ok then S.status = fit(err, 80) else S.asked = gear.time() end
    return ok
end

local function title_of(r)
    return (type(r.name) == 'string' and r.name ~= '') and r.name or ('record ' .. tostring(r.key))
end

-- A new or changed record against the last state: a line on the screen.
local function announce(old, new)
    if not old or not old.records or not gear.settings.get('notices') then return end
    local before = {}
    for _, r in ipairs(old.records) do if type(r) == 'table' then before[tostring(r.key)] = r.value end end
    for _, r in ipairs(new.records) do
        if type(r) == 'table' and before[tostring(r.key)] ~= r.value then
            S.toast = ('Quest record: %s%s'):format(fit(title_of(r), 50), before[tostring(r.key)] == nil and ' (new)' or ' (changed)')
            S.toast_until = gear.time() + 8
            return
        end
    end
end

ch:on(function(from, p)
    if from.slot ~= 0 or type(p) ~= 'table' then return end          -- only the server answers
    if p.op == 'state' then
        if p.available == true and type(p.records) == 'table' then
            local new = {total = tonumber(p.total) or #p.records, known = tonumber(p.known) or 0, records = p.records, catalog = tonumber(p.catalog) or 0}
            announce(S.state, new)
            S.state, S.reason = new, nil
        else
            S.state, S.reason = nil, fit(p.reason or 'the server has no quest data for you', 160)
        end
        S.status = ''
    elseif p.op == 'error' then
        S.status = fit(p.error, 80)
    end
end)

-- ---- quest items (read here) ----
local function refresh_near()
    S.near_at = gear.time()
    local hero = gear.world.hero()
    S.near = {}
    if not hero then return end
    local hx, hy, layer = hero:position()
    if not hx then return end
    for _, it in ipairs(gear.world.items_near(hx, hy, NEAR_TILES * gear.world.TILE, {layer = layer, limit = 64})) do
        if it.quest then
            local dist = math.sqrt((it.x - hx) ^ 2 + (it.y - hy) ^ 2) / gear.world.TILE
            S.near[#S.near + 1] = {name = it.type_name or ('item ' .. tostring(it.type)), dist = dist, x = it.x, y = it.y}
        end
    end
    table.sort(S.near, function(a, b) return a.dist < b.dist end)
end

local function backpack_quest_items()
    local list = {}
    local c = gear.inv.container('backpack')
    if not c then return list end
    for _, it in ipairs(c:items()) do
        if it.quest then list[#list + 1] = it end
    end
    return list
end

local function items_tab(x, y, w, h)
    if gear.time() - S.near_at > 1 then refresh_near() end
    ui.text('In your backpack', x, y, {color = ui.COLORS.accent})
    local cy = y + 18
    local bag = backpack_quest_items()
    if #bag == 0 then ui.text('no quest items', x + 8, cy, {color = DIM}); cy = cy + 16 end
    for _, it in ipairs(bag) do
        ui.text(('%s  (%d x %d cells)'):format(fit(it.type_name or ('item ' .. tostring(it.type)), 40), it.w or 1, it.h or 1), x + 8, cy, {})
        cy = cy + 16
    end
    cy = cy + 10
    ui.text(('On the ground within %d tiles'):format(NEAR_TILES), x, cy, {color = ui.COLORS.accent})
    cy = cy + 18
    if #S.near == 0 then ui.text('no quest items in sight', x + 8, cy, {color = DIM}) end
    local area = ui.scroll_begin('near', x, cy, w, math.max(40, y + h - cy), {step = 16})
    for i, n in ipairs(S.near) do
        ui.text(('%s  -  %.0f tiles away, at %d, %d'):format(fit(n.name, 36), n.dist, n.x, n.y), x + 8, area.content_y + (i - 1) * 16, {})
    end
    ui.scroll_end(#S.near * 16)
end

-- ---- records (from the server) ----
local function records_tab(x, y, w, h)
    if not net.supported() then
        local lines = ui.text_wrap('Records need a game server with the quest helper script (Sacred.GameServer of the SacredEngineRemake, scripts-examples/quest_helper.lua). '
            .. 'Single player and the original GameServer.exe have none.', w, {})
        for i, line in ipairs(lines) do ui.text(line, x, y + (i - 1) * 16, {color = WARN}) end
        return
    end
    if not S.state then
        local lines = ui.text_wrap(S.reason or 'Asking the server ...', w, {})
        for i, line in ipairs(lines) do ui.text(line, x, y + (i - 1) * 16, {color = S.reason and WARN or DIM}) end
        if ui.button('ask', x, y + #lines * 16 + 8, 80, 20, 'Refresh') then ask(true) end
        return
    end
    local st = S.state
    ui.text(('%d record%s, %d with a catalog entry (the catalog has %d)'):format(st.total, st.total == 1 and '' or 's', st.known, st.catalog), x, y, {color = DIM})
    local _
    _, S.show_unknown = ui.checkbox('unknown', x + w - 190, y - 2, 'Show unnamed records', S.show_unknown, {})
    if ui.button('refresh', x + w - 70, y + 20, 70, 20, 'Refresh') then ask(true) end
    local area = ui.scroll_begin('records', x, y + 22, w - 76, h - 24, {step = 20})
    local cy = area.content_y
    local shown = 0
    for _, r in ipairs(st.records) do
        if type(r) == 'table' and (r.known or S.show_unknown) then
            shown = shown + 1
            local label = (type(r.label) == 'string' and r.label ~= '') and (' - ' .. fit(r.label, 40)) or ''
            ui.text(('%s = %s%s'):format(fit(title_of(r), 50), tostring(r.value), label), x, cy, {color = r.known and GOLD or nil})
            cy = cy + 16
            if type(r.area) == 'string' and r.area ~= '' then ui.text('Where: ' .. fit(r.area, 80), x + 10, cy, {color = DIM}); cy = cy + 16 end
            if type(r.hint) == 'string' and r.hint ~= '' then
                local lines = ui.text_wrap(fit(r.hint, 400), area.w - 24, {})
                for _, line in ipairs(lines) do ui.text(line, x + 10, cy, {}); cy = cy + 15 end
            end
            if type(r.others) == 'table' and #r.others > 0 then
                local names = {}
                for _, n in ipairs(r.others) do names[#names + 1] = fit(n, 20) end
                ui.text('Also: ' .. table.concat(names, ', '), x + 10, cy, {color = OK}); cy = cy + 16
            end
            cy = cy + 6
        end
    end
    if shown == 0 then ui.text('nothing to show (the server has no quest records of your hero, or they are hidden)', x, cy, {color = DIM}); cy = cy + 16 end
    ui.scroll_end(cy - area.content_y)
end

local ABOUT = 'What this panel can show: quest items (backpack and near you, read by this mod) and the quest records of your hero (reported by the server). '
    .. 'What it cannot: the game\'s quest journal, active or available quests and their progress. Gear has no access to them and the server cannot read them: '
    .. 'a hero file holds only [key][value] quest pairs, the journal is state of the game\'s script interpreter. Names, areas and hints for a record key '
    .. 'exist only where the server owner added them to the server\'s catalog; every other record is shown as "record <key>". '
    .. 'The records need a Sacred.GameServer with quest_helper.lua that keeps hero data (web UI or --authoritative).'

local function about_tab(x, y, w, h)
    local lines = ui.text_wrap(ABOUT, w, {})
    for i, line in ipairs(lines) do ui.text(line, x, y + (i - 1) * 16, {}) end
end

local function panel()
    local p = ui.panel('main', 'Quest helper (F8)', 80, 60, 520, 400, {})
    if not p.open then
        if S.opened then S.opened = false; if net.supported() then ch:send({op = 'unwatch'}, 'server') end end
        return
    end
    if not S.opened then S.opened = true; refresh_near(); ask(true) end
    local x, y, w, h = p.content_x, p.content_y, p.content_w, p.content_h
    local _, tab = ui.radio_group('tabs', x, y, TABS, S.tab, {horizontal = true})
    S.tab = tab
    local top, body_h = y + 28, h - 28 - 20
    if S.tab == 1 then items_tab(x, top, w, body_h)
    elseif S.tab == 2 then records_tab(x, top, w, body_h)
    else about_tab(x, top, w, body_h) end
    if S.status ~= '' then ui.text(S.status, x, y + h - 16, {color = WARN}) end
end

gear.keys.on('toggle', function() ui.show_panel('main', not ui.panel_open('main')) end)

-- Refresh while the panel is open (the server also pushes changes to a watching client).
gear.every(5, function()
    if ui.panel_open('main') and net.supported() and gear.time() - S.asked >= (tonumber(gear.settings.get('refresh')) or 30) then ask(true) end
end)

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    if S.toast and gear.time() < S.toast_until and not state.escape_menu then
        ui.text(S.toast, 0, 110, {width = 1024, align = 'center', color = GOLD, background = ui.rgba(0, 0, 0, 150), padding = 4, layer = 'top'})
    end
    if state.exclusive then return end
    panel()
end)

gear.on('net_caps', function(caps)
    if caps.supported and ui.panel_open('main') then ask(true) end
end)
