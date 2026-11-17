-- Session tracker: time played, kills, gold and experience gained this session, and what they come to per hour.
-- A panel (F2) shows the session next to the lifetime totals and the last sessions; "New session" starts a new one.
--
-- What it counts, and what it cannot know:
--   * Time: real seconds with a hero in the game and the game screen up (not in the escape menu or a full screen window).
--   * Kills: the 'kill' event, the kills the local hero made. It reaches Lua from the net messages the game sends
--     (docs/GearServerMods.md); it has not been confirmed to fire in a single player game, so "Kills" may stay 0 there.
--   * Gold / experience: the hero's gold and experience polled twice a second. "Gained" is the sum of the rises (a
--     sale to a merchant counts as a gain, a purchase as "spent"); a fall of experience is not subtracted.
--   * Levels: rises of the hero's level.
-- A session is one game with a hero: 'session.start' begins one (the previous is put in the history), the button
-- begins one by hand. Totals and history are written to session_totals.json in the mod's folder.

local ui = gear.ui
local W = gear.world
local FILE = 'session_totals.json'
local POLL = 0.5
local HISTORY_MAX = 10

local function setting(id, fallback)
    local ok, v = pcall(gear.settings.get, id)
    if ok and type(v) == 'number' then return v end
    return fallback
end

local function new_stats()
    return {played = 0, kills = 0, gold_gained = 0, gold_spent = 0, xp_gained = 0, levels = 0}
end
local FIELDS = {'played', 'kills', 'gold_gained', 'gold_spent', 'xp_gained', 'levels'}

local session                  -- this session's counters (nil before a hero is seen)
local lifetime = new_stats()   -- the totals of the finished sessions (what the file holds)
local history = {}             -- newest first: {played, kills, gold_gained, xp_gained, levels, hero}
local hero_name = nil
local base = {gold = nil, xp = nil, level = nil}
local poll_t, save_t = 0, 0
local ready_mode = true        -- the game screen is up (set by the draw handler)
local warned = {}

local function warn(where, err)
    if not warned[where] then
        warned[where] = true
        print(('sessiontracker: %s failed: %s'):format(where, tostring(err)))
    end
end

local function guard(where, fn)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then warn(where, err) end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Formatting
---------------------------------------------------------------------------------------------------------------
local function fmt_time(s)
    s = math.max(0, math.floor(tonumber(s) or 0))
    return ('%d:%02d:%02d'):format(math.floor(s / 3600), math.floor(s / 60) % 60, s % 60)
end

local function fmt_num(n)
    n = math.floor((tonumber(n) or 0) + 0.5)
    local sign = n < 0 and '-' or ''
    local out = tostring(math.abs(n)):reverse():gsub('(%d%d%d)', '%1,'):reverse()
    if out:sub(1, 1) == ',' then out = out:sub(2) end
    return sign .. out
end

-- value per hour of play, or nil while the session is too short to say
local function per_hour(value, played)
    if (played or 0) < setting('min_rate_seconds', 30) or played <= 0 then return nil end
    return value / played * 3600
end

local function fmt_rate(value, played)
    local r = per_hour(value, played)
    return r and (fmt_num(r) .. '/h') or '-'
end

---------------------------------------------------------------------------------------------------------------
-- Persistence
---------------------------------------------------------------------------------------------------------------
local function load_file()
    local text = gear.read(FILE)
    if not text then return end
    local data = gear.json.decode(text)
    if type(data) ~= 'table' then return end
    if type(data.lifetime) == 'table' then
        for _, f in ipairs(FIELDS) do
            local v = tonumber(data.lifetime[f])
            if v then lifetime[f] = v end
        end
    end
    if type(data.history) == 'table' then
        for i, h in ipairs(data.history) do
            if i > HISTORY_MAX then break end
            if type(h) == 'table' then history[#history + 1] = h end
        end
    end
end

local function save_file()
    local ok, err = gear.save(FILE, gear.json.encode({lifetime = lifetime, history = history}))
    if not ok then warn('save', err) end
end

-- Moves the session into the lifetime totals and the history (once), then drops it.
local function close_session()
    local s = session
    session = nil
    if not s then return end
    if s.played >= 1 then
        for _, f in ipairs(FIELDS) do lifetime[f] = lifetime[f] + (s[f] or 0) end
        table.insert(history, 1, {
            played = math.floor(s.played), kills = s.kills, gold_gained = s.gold_gained, xp_gained = s.xp_gained,
            levels = s.levels, hero = s.hero,
        })
        while #history > HISTORY_MAX do table.remove(history) end
        save_file()
    end
end

local function begin_session()
    close_session()
    session = new_stats()
    session.hero = hero_name
    base.gold, base.xp, base.level = nil, nil, nil
end

---------------------------------------------------------------------------------------------------------------
-- Polling the hero
---------------------------------------------------------------------------------------------------------------
local function read_hero()
    local hero = W.hero()
    if not hero then return nil end
    if hero_name == nil then
        local ok, n = pcall(hero.name, hero)
        hero_name = ok and n or nil
        if session then session.hero = session.hero or hero_name end
    end
    local function get(method)
        local ok, v = pcall(hero[method], hero)
        if ok and type(v) == 'number' then return v end
        return nil
    end
    return get('gold'), get('experience'), get('level')
end

local function poll()
    local gold, xp, level = read_hero()
    if not session then return end
    if gold ~= nil then
        if base.gold ~= nil then
            local d = gold - base.gold
            if d > 0 then session.gold_gained = session.gold_gained + d
            elseif d < 0 then session.gold_spent = session.gold_spent - d end
        end
        base.gold = gold
    end
    if xp ~= nil then
        if base.xp ~= nil and xp > base.xp then session.xp_gained = session.xp_gained + (xp - base.xp) end
        base.xp = xp
    end
    if level ~= nil then
        if base.level ~= nil and level > base.level then session.levels = session.levels + (level - base.level) end
        base.level = level
    end
end

---------------------------------------------------------------------------------------------------------------
-- Events
---------------------------------------------------------------------------------------------------------------
gear.on('load', guard('load', load_file))

gear.on('session.start', guard('session.start', function()
    hero_name = nil
    begin_session()
end))

gear.on('session.stop', guard('session.stop', function()
    close_session()
    hero_name = nil
end))

gear.on('unload', guard('unload', close_session))

gear.on('kill', guard('kill', function(kill)
    if type(kill) ~= 'table' then return end
    local mine = kill.by_local
    if mine == nil and type(kill.killer) == 'table' then mine = kill.killer.is_local end
    if mine and session then session.kills = session.kills + 1 end
end))

gear.on('frame', guard('frame', function(dt)
    dt = tonumber(dt) or 0
    if dt < 0 or dt > 5 then dt = 0 end        -- a stall or a loading screen is not play time
    if not session then
        -- The mod started in a running game without a 'session.start': begin when a hero is there.
        if W.hero() then begin_session() end
        return
    end
    if ready_mode then session.played = session.played + dt end
    poll_t = poll_t + dt
    if poll_t >= POLL then poll_t = 0; poll() end
    save_t = save_t + dt
    if save_t >= setting('save_interval', 60) then
        save_t = 0
        if session.played >= 1 then
            -- A crash must not lose the whole session: the file holds the finished sessions plus this one as it
            -- stands (the lifetime is only added once at the end, so the file's lifetime stays the finished ones').
            local ok, err = gear.save(FILE, gear.json.encode({lifetime = lifetime, history = history, current = session}))
            if not ok then warn('save', err) end
        end
    end
end))

---------------------------------------------------------------------------------------------------------------
-- The panel
---------------------------------------------------------------------------------------------------------------
local GOLD = 0xFFFFD778
local DIM = 0xFFB0B0B0

local function row(x, y, w, label, value, color)
    ui.text(label, x, y, {color = DIM})
    ui.text(value, x, y, {width = w, align = 'right', color = color})
end

local function draw_panel()
    local p = ui.panel('tracker', 'Session (F2)', 16, 200, 270, 330, {})
    if not p.open then return end
    local x, y = p.content_x or 0, p.content_y or 0
    local w = p.content_w or 250
    local s = session or new_stats()
    local accent = ui.COLORS and ui.COLORS.accent
    row(x, y, w, 'Time played', fmt_time(s.played), 0xFFFFFFFF)
    row(x, y + 18, w, 'Kills', fmt_num(s.kills) .. '  (' .. fmt_rate(s.kills, s.played) .. ')', 0xFFFFFFFF)
    row(x, y + 36, w, 'Gold gained', fmt_num(s.gold_gained) .. '  (' .. fmt_rate(s.gold_gained, s.played) .. ')', GOLD)
    row(x, y + 54, w, 'Gold spent', fmt_num(s.gold_spent), DIM)
    row(x, y + 72, w, 'Experience', fmt_num(s.xp_gained) .. '  (' .. fmt_rate(s.xp_gained, s.played) .. ')', 0xFF9CD0FF)
    row(x, y + 90, w, 'Levels gained', fmt_num(s.levels), 0xFFFFFFFF)
    ui.label('l-life', 'Lifetime (with this session)', x, y + 116, {color = accent})
    local function life(f) return lifetime[f] + (s[f] or 0) end
    row(x, y + 134, w, 'Time', fmt_time(life('played')), 0xFFFFFFFF)
    row(x, y + 152, w, 'Kills', fmt_num(life('kills')), 0xFFFFFFFF)
    row(x, y + 170, w, 'Gold gained', fmt_num(life('gold_gained')), GOLD)
    row(x, y + 188, w, 'Experience', fmt_num(life('xp_gained')), 0xFF9CD0FF)
    ui.label('l-hist', 'Last sessions', x, y + 214, {color = accent})
    for i = 1, 3 do
        local h = history[i]
        if h then
            ui.text(('%s  %s  k%s  g%s'):format(fmt_time(h.played), tostring(h.hero or '?'), fmt_num(h.kills), fmt_num(h.gold_gained)),
                x, y + 214 + i * 16, {color = DIM})
        end
    end
    if ui.button('reset', x, y + 278, 110, 22, 'New session', {tooltip = 'Ends this session (it goes to the history) and starts a new one'}) then
        begin_session()
    end
end

gear.on('draw', guard('draw', function(state)
    if type(state) ~= 'table' then return end
    ready_mode = state.mode == 'game' and not state.escape_menu and not state.exclusive
    if state.mode == 'game' and not state.exclusive then draw_panel() end
end))

ui.show_panel('tracker', true)
gear.keys.on('panel', guard('key', function()
    ui.show_panel('tracker', not ui.panel_open('tracker'))
end))
