-- Loot log: what dropped near the hero and what ended up in the backpack, filterable, kept between sessions.
-- The panel (F3) lists the entries newest first; filters: lowest quality, category, kind; "Clear" empties the log.
--
-- What the Gear API gives and how the log uses it (there is no pickup event and no "item dropped" event):
--   * drop: twice a second the mod looks at the items lying within `radius` tiles of the hero
--     (gear.world.items_near). An item that was not there before is a drop. The creature that dropped it is named
--     when the engine's loot function ran in the last three seconds (the 'drops.roll' event; host / single player
--     only), else the source is blank. Things the player drops from the backpack, chest contents and items that walk
--     into range count as drops too: the API cannot tell them apart.
--   * pickup: an item that shows up in the backpack (gear.inv.container('backpack'):items()) and was logged as lying on
--     the ground. "acquired" is an item that appears in the backpack without a logged drop (bought, moved from
--     equipment, taken from the chest ... an item that was in the chest a moment before is ignored).
--   * Items already lying around, or in the backpack, when a game starts are the baseline and not logged.
--   * Gold piles are only drops ("Log gold piles"): the pile's amount is not exposed.
-- The log is written to lootlog.json in the mod's folder.

local ui = gear.ui
local W = gear.world
local FILE = 'lootlog.json'
local SCAN = 0.5
local SAVE_EVERY = 30
local ROLL_WINDOW = 3          -- seconds after a loot roll in which a new item is attributed to its creature

local QUALITY_RANK = {normal = 1, magic = 2, rare = 3, set = 4, unique = 5}
local QUALITY_COLOR = {normal = 0xFFEFEFEF, magic = 0xFFA0A0FF, rare = 0xFFF0E040, set = 0xFF15FF25, unique = 0xFFC19A33}
local Q_OPTIONS = {'All qualities', 'Magic and better', 'Rare and better', 'Set and unique', 'Unique'}
local Q_MIN = {1, 2, 3, 4, 5}
local CATEGORIES = {'weapon', 'armor', 'shield', 'jewelry', 'potion', 'rune', 'scroll', 'special', 'gold', 'other'}
local C_OPTIONS = {'All categories'}
for _, c in ipairs(CATEGORIES) do C_OPTIONS[#C_OPTIONS + 1] = c end
local K_OPTIONS = {'Drops and pickups', 'Drops', 'Picked up', 'Acquired'}
local K_VALUE = {nil, 'drop', 'pickup', 'acquired'}
local KIND_LABEL = {drop = 'dropped', pickup = 'picked up', acquired = 'acquired'}

local entries = {}             -- oldest first: {s = session number, t = seconds into the session, kind, name, quality, category, src}
local session_no = 0
local clock = 0
local scan_t, save_t = 0, 0
local dirty = false
local filter = {q = 1, c = 1, k = 1}
local baseline = false         -- the first scan of a session only learns what is there
local ground = {}              -- id -> true: on the ground at the last scan
local logged = {}              -- id -> {type = , t = }: ids we logged as drops (pruned after ten minutes)
local pack = {}                -- id -> true: in the backpack at the last scan
local chest = {}               -- id -> true: in the chest at the last scan
local last_roll = nil          -- {creature = type id, t = clock}
local warned = {}

local function setting(id, fallback)
    local ok, v = pcall(gear.settings.get, id)
    if ok and v ~= nil then return v end
    return fallback
end

local function warn(where, err)
    if not warned[where] then
        warned[where] = true
        print(('lootlog: %s failed: %s'):format(where, tostring(err)))
    end
end

local function guard(where, fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then warn(where, err) end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Persistence
---------------------------------------------------------------------------------------------------------------
local function trim()
    local max = setting('max_entries', 500)
    while #entries > max do table.remove(entries, 1) end
end

local function load_file()
    local text = gear.read(FILE)
    if not text then return end
    local data = gear.json.decode(text)
    if type(data) ~= 'table' then return end
    session_no = tonumber(data.session) or 0
    if type(data.entries) == 'table' then
        for _, e in ipairs(data.entries) do
            if type(e) == 'table' and type(e.name) == 'string' then
                entries[#entries + 1] = {
                    s = tonumber(e.s) or 0, t = tonumber(e.t) or 0, kind = tostring(e.kind or 'drop'), name = e.name,
                    quality = QUALITY_RANK[e.quality] and e.quality or 'normal',
                    category = type(e.category) == 'string' and e.category or 'other',
                    src = type(e.src) == 'string' and e.src or nil,
                }
            end
        end
    end
    trim()
end

local function save_file()
    dirty = false
    trim()
    local ok, err = gear.save(FILE, gear.json.encode({session = session_no, entries = entries}))
    if not ok then warn('save', err) end
end

---------------------------------------------------------------------------------------------------------------
-- Entries
---------------------------------------------------------------------------------------------------------------
-- A readable name: the first line of the game's tooltip when it comes without colour codes, else the type name.
local function item_name(item)
    local name
    local ok, text = pcall(ui.item_tooltip_text, item)
    if ok and type(text) == 'string' then
        local line = text:match('^[^\r\n]+')
        if line then
            line = line:gsub('\\c%x%x%x%x%x%x%x%x', ''):gsub('^%s+', ''):gsub('%s+$', '')
            if line ~= '' and not line:find('\\', 1, true) then name = line end
        end
    end
    return name or item.type_name or ('type ' .. tostring(item.type))
end

local function worth_logging(item)
    if item.gold or item.category == 'gold' then return setting('log_gold', false) end
    if item.quest or item.set then return true end
    if (QUALITY_RANK[item.quality] or 1) > 1 then return true end
    return setting('log_normal', false)
end

local function source_name()
    if last_roll and clock - last_roll.t <= ROLL_WINDOW then
        local ok, n = pcall(function() return gear.data.object_types:name(last_roll.creature) end)
        if ok and type(n) == 'string' then return n end
    end
    return nil
end

local function add(kind, item, src)
    entries[#entries + 1] = {
        s = session_no, t = math.floor(clock), kind = kind, name = item_name(item),
        quality = QUALITY_RANK[item.quality] and item.quality or 'normal',
        category = type(item.category) == 'string' and item.category or 'other', src = src,
    }
    dirty = true
    trim()
end

---------------------------------------------------------------------------------------------------------------
-- Scanning
---------------------------------------------------------------------------------------------------------------
local function id_set(list)
    local set = {}
    for _, it in ipairs(list or {}) do
        if type(it) == 'table' and it.id then set[it.id] = it end
    end
    return set
end

local function container_items(which)
    local ok, c = pcall(gear.inv.container, which)
    if not ok or not c then return nil end
    local ok2, items = pcall(c.items, c)
    if ok2 and type(items) == 'table' then return items end
    return nil
end

local function scan()
    local hero = W.hero()
    if not hero then return end
    local x, y, layer = hero:position()
    if not x then return end

    local items = {}
    local radius = setting('radius', 25) * (W.TILE or 53.7)
    local ok, near = pcall(W.items_near, x, y, radius, {layer = layer or 0, limit = 64})
    if ok and type(near) == 'table' then items = near end
    local now_ground = id_set(items)

    local pack_items = container_items('backpack')
    local now_pack = pack_items and id_set(pack_items) or nil
    local chest_items = container_items('chest')
    local now_chest = chest_items and id_set(chest_items) or nil

    if baseline then
        for id, it in pairs(now_ground) do logged[id] = {type = it.type, t = clock} end
    else
        -- drops: lying around now, not logged before
        local src
        for id, it in pairs(now_ground) do
            local seen = logged[id]
            if not (seen and seen.type == it.type) then
                logged[id] = {type = it.type, t = clock}
                if worth_logging(it) then
                    src = src or source_name()
                    add('drop', it, src)
                end
            end
        end
        -- pickups: new in the backpack
        if now_pack then
            for id, it in pairs(now_pack) do
                if not pack[id] and not chest[id] then
                    local seen = logged[id]
                    local was_ground = ground[id] or (seen and seen.type == it.type)
                    if worth_logging(it) then add(was_ground and 'pickup' or 'acquired', it) end
                end
            end
        end
    end
    baseline = false
    ground = now_ground
    if now_pack then pack = now_pack end
    if now_chest then chest = now_chest end

    -- ids come back after an item is gone: forget the old ones
    for id, seen in pairs(logged) do
        if clock - seen.t > 600 and not now_ground[id] then logged[id] = nil end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Events
---------------------------------------------------------------------------------------------------------------
local function new_session()
    session_no = session_no + 1
    clock = 0
    baseline = true
    ground, pack, chest, logged, last_roll = {}, {}, {}, {}, nil
end

gear.on('load', guard('load', load_file))
gear.on('session.start', guard('session.start', new_session))
gear.on('session.stop', guard('session.stop', function() if dirty then save_file() end end))
gear.on('unload', guard('unload', function() if dirty then save_file() end end))

-- Runs inside the engine's loot function: only numbers are stored, nothing is looked up here.
gear.on('drops.roll', function(ev)
    if type(ev) == 'table' and type(ev.creature) == 'number' then last_roll = {creature = ev.creature, t = clock} end
    return nil
end)

gear.on('frame', guard('frame', function(dt)
    dt = tonumber(dt) or 0
    if dt < 0 or dt > 5 then dt = 0 end
    clock = clock + dt
    scan_t = scan_t + dt
    if scan_t >= SCAN then scan_t = 0; scan() end
    save_t = save_t + dt
    if save_t >= SAVE_EVERY then
        save_t = 0
        if dirty then save_file() end
    end
end))

---------------------------------------------------------------------------------------------------------------
-- The panel
---------------------------------------------------------------------------------------------------------------
local function passes(e)
    if (QUALITY_RANK[e.quality] or 1) < Q_MIN[filter.q] then return false end
    if filter.c > 1 and e.category ~= CATEGORIES[filter.c - 1] then return false end
    local kind = K_VALUE[filter.k]
    if kind and e.kind ~= kind then return false end
    return true
end

local function clock_text(s)
    s = math.max(0, math.floor(tonumber(s) or 0))
    return ('%d:%02d'):format(math.floor(s / 60), s % 60)
end

local function draw_panel()
    local p = ui.panel('log', 'Loot log (F3)', 300, 200, 420, 360, {resizable = true, min_width = 340, min_height = 220})
    if not p.open then return end
    local x, y = p.content_x or 0, p.content_y or 0
    local w = p.content_w or 400
    local h = p.content_h or 320
    local _
    _, filter.q = ui.dropdown('f-quality', x, y, 126, Q_OPTIONS, filter.q, {tooltip = 'Lowest quality shown'})
    _, filter.c = ui.dropdown('f-category', x + 132, y, 126, C_OPTIONS, filter.c, {tooltip = 'Category shown'})
    _, filter.k = ui.dropdown('f-kind', x + 264, y, 126, K_OPTIONS, filter.k, {tooltip = 'Dropped on the ground, picked up into the backpack, or acquired otherwise'})

    -- newest first
    local shown = {}
    for i = #entries, 1, -1 do
        if passes(entries[i]) then shown[#shown + 1] = entries[i] end
    end
    ui.text(('%d of %d entries'):format(#shown, #entries), x, y + 28, {color = 0xFFB0B0B0})
    if ui.button('clear', x + w - 70, y + 24, 70, 20, 'Clear', {tooltip = 'Empties the log (also the file)'}) then
        entries = {}
        dirty = true
        save_file()
    end

    local top = y + 50
    local area = ui.scroll_begin('rows', x, top, w, math.max(40, h - 50), {step = 18})
    local cy = area.content_y or top
    local ax = area.content_x or x
    local shown_n = math.min(#shown, 300)
    for i = 1, shown_n do
        local e = shown[i]
        local ry = cy + (i - 1) * 18
        local color = QUALITY_COLOR[e.quality] or 0xFFFFFFFF
        ui.text(('#%d %s'):format(e.s, clock_text(e.t)), ax, ry, {color = 0xFF808080})
        ui.text(e.name, ax + 62, ry, {color = color})
        local tail = KIND_LABEL[e.kind] or e.kind
        if e.src then tail = tail .. ' (' .. e.src .. ')' end
        ui.text(tail, ax, ry, {width = (area.w or w) - 6, align = 'right', color = 0xFFA0A0A0})
    end
    ui.scroll_end(shown_n * 18)
end

gear.on('draw', guard('draw', function(state)
    if type(state) ~= 'table' then return end
    if state.mode == 'game' and not state.exclusive then draw_panel() end
end))

ui.show_panel('log', false)
gear.keys.on('panel', guard('key', function()
    ui.show_panel('log', not ui.panel_open('log'))
end))
