-- Damage meter: damage dealt and taken by the hero, DPS, the biggest hit, damage by monster type, kills per minute.
-- The panel is on F7; the Reset button (or the 'reset' key, unbound by default) starts over.
--
-- Views (setting 'View', the View button, the 'view' key or the controller's mod menu cycle through them):
--   compact   (default) one row per character: name and DPS. Alone: a single line. In a network game every player's
--             copy of the mod sends its own numbers over the mod channel ('damagemeter'), so the party is listed;
--             players without the mod are not.
--   extended  per character: DPS, DPS taken, damage dealt; below, the biggest hit and fighting time of the local hero.
--   detailed  the original full panel of the local hero (fight and rolling DPS, totals, graph, targets, sources).
-- The panel is an ordinary Gear panel: the layout editor moves it, and its buttons are stops of the controller's D-pad.
--
-- The data: the Gear event 'damage' (a hook in the engine's damage resolution) hands over, for every hit, the final
-- damage (after resistances, before the engine applies it) and the creature TYPES of the victim and the attacker
-- (GearHit: damage, victim_type, attacker_type, site). That is all there is, and it sets the limits:
--   * Dealt = hits whose attacker type is the hero's type; taken = hits whose victim type is the hero's type. The hook
--     has no object ids, so in a network game another hero of the same class counts as the local hero, and a hit
--     between two heroes of that class counts as neither. Hits by pets, traps and effects (attacker -1 or another
--     type) are not "dealt" (the API cannot say whose pet it was); hits on the hero by them are "taken", under the
--     attacker's type name (or "environment").
--   * The number is the damage the engine computed, not the hit points taken off: overkill is not clipped to the
--     victim's life, and damage over time or reflected damage that does not pass the hook is missing.
--   * Hits the hook does not see (a different resolution path of the engine) are not counted; whether every skill goes
--     through it has not been checked in a running game.
-- Kills come from the 'kill' event (the local hero's kills; unconfirmed in single player) and give kills per minute of
-- fighting. If 'kill' never fires, that row stays 0 and everything else still works.
--
-- The 'damage' handler runs inside the engine: it does arithmetic on a few counters and returns nothing (the damage is
-- never changed).

local ui = gear.ui
local W = gear.world

local BUCKETS = 60                  -- seconds of history for the rolling DPS and the graph

local function setting(id, fallback)
    local ok, v = pcall(gear.settings.get, id)
    if ok and type(v) == 'number' then return v end
    return fallback
end

local clock = 0                     -- seconds the mod has run (sum of frame dts; the hook never calls the API for time)
local hero_type = nil               -- creature type of the local hero, refreshed every frame
local warned = {}

local T                           -- the totals, see reset()
local function reset()
    T = {
        dealt = 0, taken = 0, hits_dealt = 0, hits_taken = 0, max_dealt = 0, max_taken = 0,
        by_victim = {}, by_attacker = {},      -- type id (-1: environment) -> {damage, hits}
        kills = 0,
        fight = nil,                           -- {start, last, dealt, taken} while a fight runs
        fights = {},                           -- finished fights, newest first (at most 5)
        active = 0,                            -- seconds of finished fights
        bucket_dealt = {}, bucket_taken = {}, bucket_stamp = {},
        allies = {},                           -- ally type id -> {name, dealt, taken, hits, bucket = {}, stamp = {}, fdealt, last_dps}
    }
end
reset()

-- Allies (mercenaries, escorted NPCs such as Wilbur): creature types that fight on the hero's side. The damage hook knows
-- only types, so an ally is a TYPE: either one named in KNOWN_ALLIES, or one seen following the hero (within FOLLOW_TILES of
-- it for FOLLOW_SECONDS while the hero walked FOLLOW_WALK tiles). A type that ever hits the hero is never an ally.
local KNOWN_ALLIES = {'wilbur', 'merc'}
local FOLLOW_TILES, FOLLOW_SECONDS, FOLLOW_WALK = 9, 20, 25
local ally_types = {}               -- type id -> name (survives reset(): the knowledge is about the game, not the counters)
local hostile_types = {}            -- types that hit the hero
local seen = {}                     -- creature id -> {type, since, moved}

local function warn(where, err)
    if not warned[where] then
        warned[where] = true
        print(('damagemeter: %s failed: %s'):format(where, tostring(err)))
    end
end

local function guard(where, fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then warn(where, err) end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Counting
---------------------------------------------------------------------------------------------------------------
local function bucket(sec)
    local i = sec % BUCKETS + 1
    if T.bucket_stamp[i] ~= sec then
        T.bucket_stamp[i] = sec
        T.bucket_dealt[i], T.bucket_taken[i] = 0, 0
    end
    return i
end

local function touch_fight()
    local f = T.fight
    if not f then
        f = {start = clock, last = clock, dealt = 0, taken = 0}
        T.fight = f
    end
    f.last = clock
    return f
end

local function bump(map, key, damage)
    local e = map[key]
    if not e then e = {0, 0}; map[key] = e end
    e[1] = e[1] + damage
    e[2] = e[2] + 1
end

local function record(damage, victim_type, attacker_type)
    if type(damage) ~= 'number' or damage < 0 then return end
    victim_type = tonumber(victim_type) or -1
    attacker_type = tonumber(attacker_type) or -1
    if not hero_type then return end
    local from_hero = attacker_type == hero_type
    local to_hero = victim_type == hero_type
    if to_hero and not from_hero and attacker_type >= 0 and attacker_type ~= hero_type then
        hostile_types[attacker_type] = true
        ally_types[attacker_type] = nil
    end
    if not from_hero and not to_hero then
        local ally_att = ally_types[attacker_type] and not ally_types[victim_type]
        local ally_vic = ally_types[victim_type] and not ally_types[attacker_type]
        if ally_att or ally_vic then
            local id = ally_att and attacker_type or victim_type
            local a = T.allies[id]
            if not a then
                a = {name = ally_types[id], dealt = 0, taken = 0, hits = 0, bucket = {}, stamp = {}, fdealt = 0, last_dps = 0}
                T.allies[id] = a
            end
            touch_fight()
            if ally_att then
                a.dealt, a.hits = a.dealt + damage, a.hits + 1
                a.fdealt = a.fdealt + damage
                local sec = math.floor(clock)
                local i = sec % BUCKETS + 1
                if a.stamp[i] ~= sec then a.stamp[i], a.bucket[i] = sec, 0 end
                a.bucket[i] = a.bucket[i] + damage
            else
                a.taken = a.taken + damage
            end
        end
        return
    end
    if from_hero == to_hero then return end      -- hero against its own type: ambiguous
    local f = touch_fight()
    local b = bucket(math.floor(clock))
    if from_hero then
        T.dealt = T.dealt + damage
        T.hits_dealt = T.hits_dealt + 1
        if damage > T.max_dealt then T.max_dealt = damage end
        f.dealt = f.dealt + damage
        T.bucket_dealt[b] = T.bucket_dealt[b] + damage
        bump(T.by_victim, victim_type, damage)
    else
        T.taken = T.taken + damage
        T.hits_taken = T.hits_taken + 1
        if damage > T.max_taken then T.max_taken = damage end
        f.taken = f.taken + damage
        T.bucket_taken[b] = T.bucket_taken[b] + damage
        bump(T.by_attacker, attacker_type, damage)
    end
end

-- Runs inside the engine's damage resolution: no allocation beyond the counters, no API calls, no changes.
gear.on('damage', function(hit)
    if type(hit) == 'table' then pcall(record, hit.damage, hit.victim_type, hit.attacker_type) end
    return nil
end)

gear.on('kill', guard('kill', function(kill)
    if type(kill) ~= 'table' then return end
    local mine = kill.by_local
    if mine == nil and type(kill.killer) == 'table' then mine = kill.killer.is_local end
    if mine then T.kills = T.kills + 1 end
end))

local next_scan = 0
local last_hero_x, last_hero_y

local function matches_known(name)
    name = tostring(name or ''):lower()
    for _, k in ipairs(KNOWN_ALLIES) do if name:find(k, 1, true) then return true end end
    return false
end

-- Once a second: which creature types near the hero are following it?
local function scan_allies(hero)
    local hx, hy = hero:position()
    if not hx then return end
    local walked = 0
    if last_hero_x then walked = math.sqrt((hx - last_hero_x) ^ 2 + (hy - last_hero_y) ^ 2) end
    last_hero_x, last_hero_y = hx, hy
    local tile = W.TILE or 53.7
    local alive = {}
    local list = W.creatures_near(hx, hy, FOLLOW_TILES * tile, {limit = 32}) or {}
    for _, c in ipairs(list) do
        local ok, t = pcall(c.type, c)
        if ok and type(t) == 'number' and t ~= hero_type and not hostile_types[t] then
            local id = c.id
            local rec = seen[id]
            if not rec or rec.type ~= t then rec = {type = t, since = clock, moved = 0}; seen[id] = rec end
            rec.moved = rec.moved + walked
            alive[id] = true
            if not ally_types[t] then
                local okn, name = pcall(c.type_name, c)
                if okn and matches_known(name) then
                    ally_types[t] = tostring(name):gsub('%.GRN$', ''):gsub('_', ' ')
                elseif clock - rec.since >= FOLLOW_SECONDS and rec.moved >= FOLLOW_WALK * tile then
                    ally_types[t] = okn and type(name) == 'string' and (name:gsub('%.GRN$', ''):gsub('_', ' ')) or ('type ' .. t)
                end
            end
        end
    end
    for id in pairs(seen) do if not alive[id] then seen[id] = nil end end
end

local function end_fight()
    local f = T.fight
    if not f then return end
    T.fight = nil
    local dur = math.max(1, f.last - f.start)
    T.active = T.active + dur
    table.insert(T.fights, 1, {dur = dur, dealt = f.dealt, taken = f.taken})
    for _, a in pairs(T.allies) do a.last_dps, a.fdealt = a.fdealt / dur, 0 end
    while #T.fights > 5 do table.remove(T.fights) end
end

gear.on('frame', guard('frame', function(dt)
    dt = tonumber(dt) or 0
    if dt < 0 or dt > 5 then dt = 0 end
    clock = clock + dt
    local hero = W.hero()
    if hero then
        local ok, t = pcall(hero.type, hero)
        hero_type = ok and type(t) == 'number' and t or nil
    else
        hero_type = nil
    end
    local f = T.fight
    if f and clock - f.last > setting('combat_timeout', 5) then end_fight() end
    if hero and clock >= next_scan then
        next_scan = clock + 1
        pcall(scan_allies, hero)
    end
end))

gear.on('session.start', guard('session.start', function() reset(); seen = {}; hostile_types = {}; ally_types = {} end))

---------------------------------------------------------------------------------------------------------------
-- Derived numbers
---------------------------------------------------------------------------------------------------------------
-- Damage of the last `window` seconds, per second; while the fight is younger than the window the fight's age counts.
local function rolling(buckets)
    local window = math.min(BUCKETS, setting('window', 10))
    local now = math.floor(clock)
    local sum = 0
    for s = now - window + 1, now do
        local i = s % BUCKETS + 1
        if T.bucket_stamp[i] == s then sum = sum + (buckets[i] or 0) end
    end
    local f = T.fight
    local span = window
    if f then span = math.max(1, math.min(window, clock - f.start)) end
    return sum / span
end

local function fight_dps(f)
    if not f then return 0, 0 end
    local dur = math.max(1, f.last - f.start)
    return f.dealt / dur, f.taken / dur
end

local function total_active()
    local a = T.active
    if T.fight then a = a + math.max(1, T.fight.last - T.fight.start) end
    return a
end

local function type_name(id)
    if id == -1 then return 'environment' end
    local ok, n = pcall(function() return gear.data.object_types:name(id) end)
    if ok and type(n) == 'string' and n ~= '' then return n end
    return 'type ' .. tostring(id)
end

-- The `n` biggest entries of a by-type map: {{name, damage, hits}}.
local function top(map, n)
    local list = {}
    for id, e in pairs(map) do list[#list + 1] = {id = id, damage = e[1], hits = e[2]} end
    table.sort(list, function(a, b) if a.damage ~= b.damage then return a.damage > b.damage end return a.id < b.id end)
    local out = {}
    for i = 1, math.min(n, #list) do
        out[i] = {name = type_name(list[i].id), damage = list[i].damage, hits = list[i].hits}
    end
    return out
end

local function num(n) return tostring(math.floor((tonumber(n) or 0) + 0.5)) end


local function short(n)
    n = tonumber(n) or 0
    if n >= 1e6 then return ('%.1fM'):format(n / 1e6) end
    if n >= 1e4 then return ('%.1fk'):format(n / 1e3) end
    return num(n)
end

---------------------------------------------------------------------------------------------------------------
-- Views and sharing
---------------------------------------------------------------------------------------------------------------
local VIEWS = {'compact', 'extended', 'detailed'}
local VIEW_LABEL = {compact = 'Compact', extended = 'Extended', detailed = 'Detailed'}

local function valid_view(v)
    for _, name in ipairs(VIEWS) do if v == name then return name end end
    return nil
end

local view = 'compact'
local function load_view()
    local ok, text = pcall(gear.read, 'view.txt')       -- the view the player last chose with the button
    local v = ok and type(text) == 'string' and valid_view((text:gsub('%s+', ''))) or nil
    if not v then
        local ok2, s = pcall(gear.settings.get, 'view')
        v = ok2 and valid_view(s) or nil
    end
    view = v or 'compact'
end
load_view()

local function set_view(v)
    view = valid_view(v) or 'compact'
    pcall(gear.save, 'view.txt', view)
end

local function next_view()
    for i, name in ipairs(VIEWS) do
        if name == view then set_view(VIEWS[i % #VIEWS + 1]); return end
    end
    set_view('compact')
end

gear.on('settings.changed', guard('settings', function(id, v)
    if id == 'view' and valid_view(v) then set_view(v) end
end))

-- What this hero shows as its DPS: the last seconds while a fight runs, else the average of the last fight.
local function current_numbers()
    local f = T.fight
    local dealt_dps, taken_dps
    if f then
        dealt_dps, taken_dps = rolling(T.bucket_dealt), rolling(T.bucket_taken)
    else
        local last = T.fights[1]
        if last then dealt_dps, taken_dps = last.dealt / last.dur, last.taken / last.dur else dealt_dps, taken_dps = 0, 0 end
    end
    local allies = {}
    for _, a in pairs(T.allies) do
        local dps = a.last_dps
        if f then
            local window = math.min(BUCKETS, setting('window', 10))
            local now, sum = math.floor(clock), 0
            for sec = now - window + 1, now do
                local i = sec % BUCKETS + 1
                if a.stamp[i] == sec then sum = sum + (a.bucket[i] or 0) end
            end
            dps = sum / math.max(1, math.min(window, clock - f.start))
        end
        allies[#allies + 1] = {name = a.name, dps = dps, tdps = 0, dealt = a.dealt, taken = a.taken, fighting = f ~= nil, ally = true}
    end
    table.sort(allies, function(x, y) if x.dealt ~= y.dealt then return x.dealt > y.dealt end return x.name < y.name end)
    return {dps = dealt_dps, tdps = taken_dps, dealt = T.dealt, taken = T.taken, fighting = f ~= nil, allies = allies}
end

local CHANNEL = 'damagemeter'
local SEND_EVERY = 1
local STALE = 15                    -- seconds without news after which a party member is dropped from the list
local peers = {}                    -- slot -> {name, dps, tdps, dealt, taken, fighting, seen}
local next_send = 0

local function mp() return gear.mp end

local function network_game()
    local m = mp()
    if not (m and m.active) then return false end
    local ok, active = pcall(m.active)
    return ok and active == true
end

local function local_name()
    local m = mp()
    local ok, p = false, nil
    if m and m.local_player then ok, p = pcall(m.local_player) end
    if ok and type(p) == 'table' and type(p.name) == 'string' and p.name ~= '' then return p.name end
    local hero = W.hero()
    if hero then
        local ok2, n = pcall(hero.name, hero)
        if ok2 and type(n) == 'string' and n ~= '' then return n end
    end
    return 'Hero'
end

local function clean_number(v)
    v = tonumber(v)
    if not v or v ~= v or v < 0 or v > 1e9 then return 0 end
    return v
end

gear.on('mp_message', guard('mp_message', function(from, channel, payload)
    if channel ~= CHANNEL or type(payload) ~= 'table' or type(from) ~= 'table' then return end
    if from.is_local or type(from.slot) ~= 'number' or from.slot < 1 then return end
    peers[from.slot] = {
        name = type(from.name) == 'string' and from.name or ('hero ' .. tostring(from.slot)),
        dps = clean_number(payload.dps), tdps = clean_number(payload.tdps),
        dealt = clean_number(payload.dealt), taken = clean_number(payload.taken),
        fighting = payload.fighting == true, seen = clock, allies = {},
    }
    if type(payload.allies) == 'table' then
        local mine = peers[from.slot].allies
        for i = 1, math.min(4, #payload.allies) do
            local a = payload.allies[i]
            if type(a) == 'table' and type(a.name) == 'string' then
                mine[#mine + 1] = {name = a.name:sub(1, 24), dps = clean_number(a.dps), tdps = 0, dealt = clean_number(a.dealt),
                    taken = 0, fighting = payload.fighting == true, ally = true}
            end
        end
    end
end))

-- Sends the local numbers once a second while other players are in the game.
local function share()
    if not network_game() then peers = {}; return end
    local ok, on = pcall(gear.settings.get, 'share')
    if ok and on == false then return end
    if clock < next_send then return end
    next_send = clock + SEND_EVERY
    local n = current_numbers()
    local allies = {}
    for i = 1, math.min(4, #n.allies) do
        local a = n.allies[i]
        allies[i] = {name = a.name, dps = a.dps, dealt = a.dealt}
    end
    pcall(mp().send, CHANNEL, {dps = n.dps, tdps = n.tdps, dealt = n.dealt, taken = n.taken, fighting = n.fighting, allies = allies}, 'all')
end

gear.on('frame', guard('share', function()
    if hero_type then share() end
    for slot, e in pairs(peers) do if clock - e.seen > STALE then peers[slot] = nil end end
end))

-- The characters to list, sorted by damage dealt: {name, dps, tdps, dealt, taken, fighting, mine}.
local function characters()
    local list = {}
    if hero_type then
        local n = current_numbers()
        n.name, n.mine = local_name(), true
        list[1] = n
    end
    if network_game() then
        for _, e in pairs(peers) do list[#list + 1] = e end
    end
    table.sort(list, function(a, b)
        if a.dealt ~= b.dealt then return a.dealt > b.dealt end
        if (a.mine or false) ~= (b.mine or false) then return a.mine or false end
        return a.name < b.name
    end)
    -- Each character is followed by its allies (mercenaries, NPC companions) as rows of their own.
    local rows = {}
    for _, c in ipairs(list) do
        rows[#rows + 1] = c
        for _, a in ipairs(c.allies or {}) do rows[#rows + 1] = a end
    end
    return rows
end

local function fit_name(name, max)
    name = tostring(name or '?')
    if #name > max then return name:sub(1, max - 1) .. '.' end
    return name
end

---------------------------------------------------------------------------------------------------------------
-- The panel
---------------------------------------------------------------------------------------------------------------
local DIM = 0xFFB0B0B0
local RED = 0xFFFF8080
local GREEN = 0xFF90E090
local WHITE = 0xFFFFFFFF
local GOLD = 0xFFFFE6A0
local MAX_ROWS = 8
local ROW = 17

local function row(x, y, w, label, value, color)
    ui.text(label, x, y, {color = DIM})
    ui.text(value, x, y, {width = w, align = 'right', color = color or WHITE})
end

-- Height of a view's content, from the number of characters.
local function content_height(n)
    local rows = math.max(1, math.min(MAX_ROWS, n))
    if view == 'compact' then return n <= 1 and not network_game() and 38 or rows * ROW + 4 end
    if view == 'extended' then return 16 + rows * ROW + 6 + 3 * ROW + 4 end
    return 366
end

-- The footer shared by every view: View cycles the views, Reset clears the counters.
local function footer(x, y, w)
    local vw = math.min(78, w - 54 - 8)          -- the compact panel is narrow
    if ui.button('view', x, y, vw, 18, VIEW_LABEL[view], {tooltip = 'Next view (compact, extended, detailed)'}) then next_view() end
    if ui.button('reset', x + w - 54, y, 54, 18, 'Reset', {tooltip = 'Clears every counter'}) then reset() end
end

local function draw_compact(x, y, w, list)
    if #list == 0 then
        ui.text('no hero', x, y, {color = DIM})
    elseif #list == 1 and not network_game() then
        -- Alone: one line, the number big enough to read in a fight.
        local e = list[1]
        ui.text(fit_name(e.name, 16), x, y + 2, {color = DIM})
        ui.text(num(e.dps), x, y, {width = w, align = 'right', color = e.fighting and GREEN or WHITE, size = 18})
        ui.text('DPS', x, y + 20, {width = w, align = 'right', color = DIM, size = 11})
    else
        for i = 1, math.min(MAX_ROWS, #list) do
            local e = list[i]
            local yy = y + (i - 1) * ROW
            ui.text(e.ally and ('  ' .. fit_name(e.name, 12)) or fit_name(e.name, 14), x, yy, {color = e.ally and DIM or (e.mine and GOLD or WHITE)})
            ui.text(num(e.dps), x, yy, {width = w, align = 'right', color = e.fighting and GREEN or DIM})
        end
    end
end

local function draw_extended(x, y, w, list)
    if #list == 0 then
        ui.text('no hero', x, y, {color = DIM})
        return
    end
    local right = x + w
    ui.text('DPS', right - 210, y, {width = 60, align = 'right', color = DIM, size = 11})
    ui.text('Taken', right - 140, y, {width = 60, align = 'right', color = DIM, size = 11})
    ui.text('Dealt', right - 70, y, {width = 70, align = 'right', color = DIM, size = 11})
    local n = math.min(MAX_ROWS, #list)
    for i = 1, n do
        local e = list[i]
        local yy = y + 16 + (i - 1) * ROW
        ui.text(e.ally and ('  ' .. fit_name(e.name, 10)) or fit_name(e.name, 12), x, yy, {color = e.ally and DIM or (e.mine and GOLD or WHITE)})
        ui.text(num(e.dps), right - 210, yy, {width = 60, align = 'right', color = e.fighting and GREEN or WHITE})
        ui.text(num(e.tdps), right - 140, yy, {width = 60, align = 'right', color = RED})
        ui.text(short(e.dealt), right - 70, yy, {width = 70, align = 'right', color = DIM})
    end
    local yy = y + 16 + n * ROW + 6
    local active = total_active()
    row(x, yy, w, 'Biggest hit (you)', num(T.max_dealt))
    row(x, yy + ROW, w, 'Fighting time (you)', num(active) .. ' s')
    row(x, yy + 2 * ROW, w, 'Average DPS (you)', active > 0 and num(T.dealt / active) or '-')
end

local function draw_detailed(x, y, w)
    local accent = ui.COLORS and ui.COLORS.accent
    local f = T.fight
    local fd, ft = fight_dps(f)
    ui.label('l-fight', f and 'Fight (running)' or 'Fight (last)', x, y, {color = accent})
    local last = T.fights[1]
    if not f and last then fd, ft = last.dealt / last.dur, last.taken / last.dur end
    row(x, y + 18, w, 'Dealt DPS / taken DPS', num(fd) .. ' / ' .. num(ft))
    row(x, y + 36, w, 'Last ' .. num(setting('window', 10)) .. ' s', num(rolling(T.bucket_dealt)) .. ' / ' .. num(rolling(T.bucket_taken)))

    ui.label('l-total', 'Since reset', x, y + 62, {color = accent})
    row(x, y + 80, w, 'Damage dealt', num(T.dealt) .. '  (' .. num(T.hits_dealt) .. ' hits)', GREEN)
    row(x, y + 98, w, 'Damage taken', num(T.taken) .. '  (' .. num(T.hits_taken) .. ' hits)', RED)
    row(x, y + 116, w, 'Biggest hit dealt / taken', num(T.max_dealt) .. ' / ' .. num(T.max_taken))
    local avg = T.hits_dealt > 0 and T.dealt / T.hits_dealt or 0
    row(x, y + 134, w, 'Average hit dealt', num(avg))
    local active = total_active()
    row(x, y + 152, w, 'Fighting time', num(active) .. ' s')
    row(x, y + 170, w, 'Average DPS while fighting', active > 0 and num(T.dealt / active) or '-')
    row(x, y + 188, w, 'Kills', num(T.kills) .. (active >= 10 and T.kills > 0 and ('  (' .. ('%.1f'):format(T.kills / active * 60) .. '/min)') or ''))

    -- The last 30 seconds as bars: dealt up (green), taken down (red), scaled to the biggest second.
    local gy, gh, gw = y + 214, 44, math.min(w, 180)
    ui.rect(x, gy, gw, gh, ui.rgba(0, 0, 0, 90))
    local now = math.floor(clock)
    local peak = 1
    for s = now - 29, now do
        local i = s % BUCKETS + 1
        if T.bucket_stamp[i] == s then peak = math.max(peak, T.bucket_dealt[i] or 0, T.bucket_taken[i] or 0) end
    end
    local bw = gw / 30
    for k = 0, 29 do
        local s = now - 29 + k
        local i = s % BUCKETS + 1
        if T.bucket_stamp[i] == s then
            local d = (T.bucket_dealt[i] or 0) / peak * (gh / 2)
            local t = (T.bucket_taken[i] or 0) / peak * (gh / 2)
            if d >= 1 then ui.rect(x + k * bw, gy + gh / 2 - d, math.max(1, bw - 1), d, ui.rgba(120, 220, 120, 220)) end
            if t >= 1 then ui.rect(x + k * bw, gy + gh / 2, math.max(1, bw - 1), t, ui.rgba(240, 100, 100, 220)) end
        end
    end
    ui.text('30 s, peak ' .. num(peak), x + gw + 6, gy, {color = DIM})

    ui.label('l-top', 'Biggest targets', x, y + 266, {color = accent})
    for i, e in ipairs(top(T.by_victim, 3)) do
        row(x, y + 266 + i * 16, w, e.name, num(e.damage) .. ' (' .. num(e.hits) .. ')')
    end
    ui.label('l-src', 'Biggest damage sources', x, y + 330, {color = accent})
    for i, e in ipairs(top(T.by_attacker, 2)) do
        row(x, y + 330 + i * 16, w, e.name, num(e.damage) .. ' (' .. num(e.hits) .. ')')
    end
end

-- Panel width (layout units) per view; the height is the content plus the title bar and the footer.
local WIDTH = {compact = 150, extended = 290, detailed = 290}
local CHROME = 56

local function draw_panel()
    local list = characters()
    local content_h = content_height(#list)
    local title = view == 'compact' and 'DPS' or 'Damage meter (F7)'
    local p = ui.panel('meter', title, 700, 200, WIDTH[view], content_h + CHROME, {})
    if not p.open then return end
    local x, y = p.content_x or 0, p.content_y or 0
    local w = p.content_w or (WIDTH[view] - 20)
    if view == 'compact' then
        draw_compact(x, y, w, list)
    elseif view == 'extended' then
        draw_extended(x, y, w, list)
    elseif hero_type then
        draw_detailed(x, y, w)
    else
        ui.text('no hero', x, y, {color = DIM})
    end
    footer(x, y + content_h + 4, w)
end

gear.on('draw', guard('draw', function(state)
    if type(state) ~= 'table' then return end
    if state.mode == 'game' and not state.exclusive then draw_panel() end
end))

ui.show_panel('meter', false)
gear.keys.on('panel', guard('key', function()
    ui.show_panel('meter', not ui.panel_open('meter'))
end))
gear.keys.on('reset', guard('key', reset))
gear.keys.on('view', guard('key', next_view))

-- The controller reaches the same things through its mod menu.
if ui.menu_item then
    pcall(ui.menu_item, 'meter', 'Damage meter: show / hide', function() ui.show_panel('meter', not ui.panel_open('meter')) end)
    pcall(ui.menu_item, 'meter_view', 'Damage meter: next view', next_view)
end
