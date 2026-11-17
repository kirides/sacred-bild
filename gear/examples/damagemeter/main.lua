-- Damage meter: damage dealt and taken by the hero, DPS, the biggest hit, damage by monster type, kills per minute.
-- The panel is on F7; the Reset button (or the 'reset' key, unbound by default) starts over.
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
    }
end
reset()

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
    if from_hero == to_hero then return end      -- neither, or hero against its own type: ambiguous
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

local function end_fight()
    local f = T.fight
    if not f then return end
    T.fight = nil
    local dur = math.max(1, f.last - f.start)
    T.active = T.active + dur
    table.insert(T.fights, 1, {dur = dur, dealt = f.dealt, taken = f.taken})
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
end))

gear.on('session.start', guard('session.start', function() reset() end))

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

---------------------------------------------------------------------------------------------------------------
-- The panel
---------------------------------------------------------------------------------------------------------------
local DIM = 0xFFB0B0B0
local RED = 0xFFFF8080
local GREEN = 0xFF90E090

local function row(x, y, w, label, value, color)
    ui.text(label, x, y, {color = DIM})
    ui.text(value, x, y, {width = w, align = 'right', color = color or 0xFFFFFFFF})
end

local function draw_panel()
    local p = ui.panel('meter', 'Damage meter (F7)', 700, 200, 290, 420, {})
    if not p.open then return end
    local x, y = p.content_x or 0, p.content_y or 0
    local w = p.content_w or 270
    local accent = ui.COLORS and ui.COLORS.accent
    if not hero_type then
        ui.text('no hero', x, y, {color = DIM})
        return
    end
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
    local targets = top(T.by_victim, 3)
    for i, e in ipairs(targets) do
        row(x, y + 266 + i * 16, w, e.name, num(e.damage) .. ' (' .. num(e.hits) .. ')')
    end
    ui.label('l-src', 'Biggest damage sources', x, y + 330, {color = accent})
    local sources = top(T.by_attacker, 2)
    for i, e in ipairs(sources) do
        row(x, y + 330 + i * 16, w, e.name, num(e.damage) .. ' (' .. num(e.hits) .. ')')
    end
    if ui.button('reset', x + w - 70, y + 354, 70, 20, 'Reset', {tooltip = 'Clears every counter'}) then reset() end
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
