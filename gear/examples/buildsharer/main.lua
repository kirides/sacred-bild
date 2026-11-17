-- Build sharer: a panel (F7) to publish your hero build to the game server and to browse the builds other players shared.
-- Server half: SacredEngineRemake Sacred.GameServer/scripts-examples/build_sharer.lua (channel 'buildsharer'). The server fills class,
-- level, skills and equipment from its own view of your hero; this mod sends a title, a note, tags (settings in the Mods screen) and
-- the names of the best items in your backpack. Gear cannot read a hero's skills or worn equipment, so nothing else can be sent.
-- Browsing: filter by class, sort, page; click a build to fetch it in full, and keep it in "Saved" (saved_builds.json in this mod's folder).
--
-- Nothing a server sends is trusted for more than showing it: texts are cut to a length, only the server (slot 0) is listened to.

local ui, net, json = gear.ui, gear.net, gear.json
local ch = net.channel('buildsharer')
local PAGE = tonumber(gear.config.page_size) or 12
local TABS = {'Browse', 'My build', 'Saved', 'Detail'}
local CLASSES = {'All classes', 'Seraphim', 'Gladiator', 'Battle Mage', 'Dark Elf', 'Wood Elf', 'Vampiress', 'Dwarf', 'Daemon'}
local CLASS_FILTER = {false, 1, 2, 3, 4, 5, 6, 8, 9}     -- false: all
local SORTS = {'Newest', 'Highest level'}
local SORT_KEY = {'new', 'level'}
local MAX_SAVED = 50
local GOLD, DIM, OK, WARN = ui.rgba(255, 215, 120), ui.rgba(255, 255, 255, 150), ui.rgba(143, 206, 122), ui.rgba(255, 200, 120)

local S = {
    tab = 1, class = 1, sort = 1, page = 1, list = nil, detail = nil, mine = nil, hero_data = nil,
    saved = {}, status = '', last_request = 0, opened = false, waiting = false,
}

local function say(text) S.status = tostring(text) end

-- Texts that come from outside are shown cut to a length (and only as text: gear.ui.text draws, it does not interpret).
local function fit(text, max)
    text = tostring(text == nil and '' or text):gsub('%c', ' ')
    if #text > max then text = text:sub(1, max - 1) .. '...' end
    return text
end

-- ---- saved builds (a file of this mod) ----
local function load_saved()
    local text = gear.read('saved_builds.json')
    if not text then return end
    local data = json.decode(text)
    if type(data) == 'table' and type(data.builds) == 'table' then
        for i = 1, math.min(#data.builds, MAX_SAVED) do
            local b = data.builds[i]
            if type(b) == 'table' and type(b.name) == 'string' then S.saved[#S.saved + 1] = b end
        end
    end
end

local function store_saved()
    local ok, err = gear.save('saved_builds.json', json.encode({builds = S.saved}))
    if not ok then say('could not save: ' .. tostring(err)) end
end

local function saved_index(build)
    for i, b in ipairs(S.saved) do
        if b.id == build.id and b.name == build.name then return i end
    end
end

-- ---- requests ----
local function ready()
    if net.supported() then return true end
    say('This server has no custom net support (it needs Sacred.GameServer with build_sharer.lua).')
    return false
end

local function request(op, fields)
    if not ready() then return false end
    if gear.time() - S.last_request < 0.4 then return false end        -- the server limits bursts too
    S.last_request = gear.time()
    local payload = fields or {}
    payload.op = op
    local ok, err = ch:send(payload, 'server')
    if not ok then say(err or 'not sent') end
    return ok
end

local function fetch_list()
    local payload = {sort = SORT_KEY[S.sort], page = S.page, per = PAGE}
    if CLASS_FILTER[S.class] then payload.class = CLASS_FILTER[S.class] end
    S.waiting = request('list', payload)
end

local function carried_names()
    if not gear.settings.get('share_carried') then return {} end
    local c = gear.inv.container('backpack')
    if not c then return {} end
    local items = c:items()
    table.sort(items, function(a, b) return (a.quality_rank or 0) > (b.quality_rank or 0) end)
    local names = {}
    for i = 1, math.min(#items, 12) do names[#names + 1] = fit(items[i].type_name or ('item ' .. tostring(items[i].type)), 48) end
    return names
end

local function my_tags()
    local tags = {}
    for tag in tostring(gear.settings.get('tags') or ''):gmatch('[^,]+') do
        tag = tag:match('^%s*(.-)%s*$')
        if #tag > 0 and #tags < 5 then tags[#tags + 1] = tag:sub(1, 16) end
    end
    return tags
end

local function publish()
    local hero = gear.world.hero()
    request('publish', {
        title = gear.settings.get('title'), note = gear.settings.get('note'), tags = my_tags(), carried = carried_names(),
        level = hero and hero:level() or nil,
    })
end

-- ---- what the server answers ----
ch:on(function(from, p)
    if from.slot ~= 0 or type(p) ~= 'table' then return end          -- only the server answers
    if p.op == 'list' then
        S.waiting = false
        if type(p.builds) ~= 'table' then return end
        S.list = {builds = p.builds, total = tonumber(p.total) or 0, pages = tonumber(p.pages) or 1}
        S.page = tonumber(p.page) or 1
        S.status = ''
    elseif p.op == 'build' and type(p.build) == 'table' then
        S.detail = p.build; S.tab = 4
    elseif p.op == 'mine' then
        S.mine = type(p.build) == 'table' and p.build or false
        S.hero_data = p.hero_data == true
    elseif p.op == 'published' then
        S.mine = type(p.build) == 'table' and p.build or S.mine
        say(S.mine and S.mine.verified and 'Published: the server read your hero.'
            or 'Published (the server has no hero data: class, level and your notes only).')
        S.list = nil
    elseif p.op == 'unpublished' then
        S.mine = false; S.list = nil
        say(p.removed and 'Your build is removed.' or 'You had no shared build.')
    elseif p.op == 'error' then
        S.waiting = false
        say(fit(p.error, 80))
    end
end)

-- ---- drawing ----
local function row_text(b)
    return ('%s  %s  L%s%s%s'):format(fit(b.name, 18), fit(b.class_name, 12), tostring(b.level or '?'), b.verified and '  [verified]' or '',
        (b.title and b.title ~= '') and ('  - ' .. fit(b.title, 30)) or '')
end

local function lines_of(items, fmt, x, y, w)
    for _, item in ipairs(items or {}) do
        ui.text(fit(fmt(item), math.floor(w / 6.5)), x, y, {})
        y = y + 16
    end
    return y
end

local function browse(x, y, w, h)
    ui.label('l-class', 'Class', x, y + 3, {})
    ui.label('l-sort', 'Sort', x + 150, y + 3, {})
    local sort_changed
    sort_changed, S.sort = ui.radio_group('sort', x + 186, y + 3, SORTS, S.sort, {horizontal = true})
    if sort_changed then S.page = 1; fetch_list() end
    if ui.button('refresh', x + w - 150, y, 70, 20, 'Refresh', {tooltip = 'Ask the server again'}) then fetch_list() end
    if ui.button('prev', x + w - 76, y, 34, 20, '<', {enabled = S.page > 1}) then S.page = S.page - 1; fetch_list() end
    if ui.button('next', x + w - 38, y, 34, 20, '>', {enabled = S.list ~= nil and S.page < S.list.pages}) then S.page = S.page + 1; fetch_list() end
    local top = y + 28
    if not S.list then
        ui.text(S.waiting and 'Asking the server ...' or 'Press Refresh to list the shared builds.', x, top, {color = DIM})
    elseif #S.list.builds == 0 then
        ui.text('No build matches. Publish yours on the "My build" tab.', x, top, {color = DIM})
    else
        local area = ui.scroll_begin('browse', x, top, w, h - 54, {step = 22})
        for i, b in ipairs(S.list.builds) do
            if type(b) == 'table' and type(b.id) == 'string' then
                if ui.button('b' .. i, x, area.content_y + (i - 1) * 22, area.w - 2, 20, row_text(b), {tooltip = 'Fetch this build'}) then
                    request('get', {id = b.id})
                end
            end
        end
        ui.scroll_end(#S.list.builds * 22)
        ui.text(('%d build%s, page %d of %d'):format(S.list.total, S.list.total == 1 and '' or 's', S.page, S.list.pages), x, y + h - 20, {color = DIM})
    end
    -- drawn last: the open list covers what is under it
    local class_changed
    class_changed, S.class = ui.dropdown('class', x + 40, y, 100, CLASSES, S.class, {tooltip = 'Only this class'})
    if class_changed then S.page = 1; fetch_list() end
end

local function my_build(x, y, w, h)
    local hero = gear.world.hero()
    ui.text('What you publish', x, y, {color = ui.COLORS.accent})
    local cls = hero and hero:class() or '?'
    ui.text(('%s, %s, level %s'):format(hero and hero:name() or '(no hero)', cls, hero and tostring(hero:level()) or '?'), x, y + 18, {})
    ui.text('The server reads your skills and equipment itself.', x, y + 36, {color = DIM})
    ui.text('Title: ' .. fit(gear.settings.get('title'), 40), x, y + 58, {})
    ui.text('Note: ' .. fit(gear.settings.get('note'), 70), x, y + 74, {})
    ui.text('Tags: ' .. table.concat(my_tags(), ', '), x, y + 90, {})
    ui.text('(change these in the Mods screen, F11, under Build sharer)', x, y + 106, {color = DIM})
    local carried = carried_names()
    ui.text(('Carried items sent: %d%s'):format(#carried, gear.settings.get('share_carried') and '' or ' (off)'), x, y + 128, {})
    if ui.button('publish', x, y + 152, 120, 24, 'Publish', {tooltip = 'Share (or update) your build on this server'}) then publish() end
    if ui.button('unpublish', x + 128, y + 152, 120, 24, 'Remove mine', {enabled = S.mine ~= nil and S.mine ~= false, tooltip = 'Take your build off the server'}) then
        request('unpublish')
    end
    if ui.button('mine', x + 256, y + 152, 120, 24, 'Check server', {tooltip = 'What the server holds of yours'}) then request('mine') end
    local ty = y + 190
    if S.mine == false then
        ui.text('The server has no build of yours.', x, ty, {color = DIM})
    elseif S.mine then
        local b = S.mine
        ui.text(('On the server: %s, level %s, %s'):format(fit(b.class_name, 14), tostring(b.level), b.verified and 'server-verified' or 'reported only'), x, ty,
            {color = b.verified and OK or DIM})
        ui.text(('%d skills, %d equipped items, %d combat arts'):format(#(b.skills or {}), #(b.equipment or {}), #(b.arts or {})), x, ty + 16, {color = DIM})
    end
    if S.hero_data == false then
        ui.text('This server keeps no hero data of you yet: your build will only have class and level.', x, ty + 40, {color = WARN})
    end
end

local function detail(x, y, w, h)
    local b = S.detail
    if not b then ui.text('Pick a build on the Browse or Saved tab.', x, y, {color = DIM}); return end
    ui.text(('%s - %s, level %s'):format(fit(b.name, 24), fit(b.class_name, 14), tostring(b.level)), x, y, {color = GOLD})
    ui.text(b.verified and 'server-verified' or 'reported by the player', x + w - 150, y, {color = b.verified and OK or DIM})
    local text = {}
    if b.title and b.title ~= '' then text[#text + 1] = fit(b.title, 60) end
    if b.note and b.note ~= '' then text[#text + 1] = fit(b.note, 240) end
    local lines, th = ui.text_wrap(table.concat(text, '\n'), w - 8, {})
    for i, line in ipairs(lines) do ui.text(line, x, y + 20 + (i - 1) * (th / math.max(1, #lines)), {}) end
    local top = y + 24 + th
    local at = saved_index(b)
    if ui.button('save', x + w - 130, top - 4, 126, 20, at and 'Remove from Saved' or 'Save to Saved', {tooltip = 'Keep this build in a file on this computer'}) then
        if at then table.remove(S.saved, at) else
            if #S.saved >= MAX_SAVED then table.remove(S.saved, 1) end
            S.saved[#S.saved + 1] = b
        end
        store_saved()
    end
    local area = ui.scroll_begin('detail', x, top + 20, w, h - (top + 20 - y) - 4, {step = 16})
    local cy = area.content_y
    local function section(title, items, fmt)
        if type(items) ~= 'table' or #items == 0 then return end
        ui.text(title, x, cy, {color = ui.COLORS.accent}); cy = cy + 18
        cy = lines_of(items, fmt, x + 8, cy, area.w - 16) + 4
    end
    section('Skills', b.skills, function(s) return ('%s  %s%s'):format(fit(s.name, 30), tostring(s.level), (s.bonus or 0) > 0 and (' (+' .. tostring(s.bonus) .. ')') or '') end)
    section('Combat arts', b.arts, function(a) return fit(a.name, 40) end)
    section('Equipment', b.equipment, function(e)
        return ('%s  [%s, item level %s%s]'):format(fit(e.name, 36), fit(e.tier, 8), tostring(e.level), (e.sockets or 0) > 0 and (', ' .. tostring(e.sockets) .. ' socketed') or '')
    end)
    section('Carried (reported)', b.carried, function(c) return fit(c, 48) end)
    if type(b.tags) == 'table' and #b.tags > 0 then
        local tags = {}
        for _, t in ipairs(b.tags) do tags[#tags + 1] = fit(t, 16) end
        ui.text('Tags: ' .. table.concat(tags, ', '), x, cy, {color = DIM}); cy = cy + 18
    end
    ui.scroll_end(cy - area.content_y)
end

local function saved_tab(x, y, w, h)
    if #S.saved == 0 then ui.text('Nothing saved yet. Open a build and use "Save to Saved".', x, y, {color = DIM}); return end
    local area = ui.scroll_begin('saved', x, y, w, h, {step = 22})
    for i, b in ipairs(S.saved) do
        if ui.button('s' .. i, x, area.content_y + (i - 1) * 22, area.w - 2, 20, row_text(b), {tooltip = 'Show this build'}) then
            S.detail = b; S.tab = 4
        end
    end
    ui.scroll_end(#S.saved * 22)
end

local function panel()
    local p = ui.panel('main', 'Build sharer (F7)', 80, 60, 560, 430, {})
    if not p.open then S.opened = false; return end
    if not S.opened then                         -- just opened: ask the server for the first page and for my own build
        S.opened = true
        if net.supported() then fetch_list(); gear.after(0.5, function() request('mine') end) else ready() end
    end
    local x, y, w, h = p.content_x, p.content_y, p.content_w, p.content_h
    local _, tab = ui.radio_group('tabs', x, y, TABS, S.tab, {horizontal = true})
    S.tab = tab
    local top, body_h = y + 26, h - 26 - 22
    if S.tab == 1 then browse(x, top, w, body_h)
    elseif S.tab == 2 then my_build(x, top, w, body_h)
    elseif S.tab == 3 then saved_tab(x, top, w, body_h)
    else detail(x, top, w, body_h) end
    if S.status ~= '' then ui.text(fit(S.status, 90), x, y + h - 16, {color = WARN}) end
end

gear.keys.on('toggle', function() ui.show_panel('main', not ui.panel_open('main')) end)

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' or state.exclusive then return end
    panel()
end)

gear.on('net_caps', function(caps)
    if caps.supported and ui.panel_open('main') and not S.list then fetch_list() end
end)

load_saved()
