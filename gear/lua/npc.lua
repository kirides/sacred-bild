-- gear.npc: custom non-player characters made of the game's own models. Shipped inside Sacred Gear and run once per mod, in the
-- mod's own environment (so `gear` below is the mod's). It only uses the public API.
--
-- An NPC kind (npc.define) names an existing object type (its model and animations), a name, dialog pages, a daily routine and a
-- look (tint). npc.spawn makes a creature of it: the creature carries the kind's name as its own name text (a text id taken from
-- the kind's id, so it is the same in every session and a savegame keeps it), which is also how a kind is recognised again after
-- a load. The creatures a mod made are listed in the mod's per-savegame data (`npc.json`).
--
-- Clicking an NPC walks the hero up to it and opens its dialog: a window with the NPC's text and the page's options. A mod reacts
-- to an option with the option's `on`, the kind's `on_talk` or an npc.on('button') handler.

local ui = gear.ui
local N = {}

local PREFIX = 'npc:'
local kinds = {}                 -- kind id -> definition
local order = {}                 -- kind ids in the order they were defined
local handlers = {talk = {}, button = {}, close = {}, arrived = {}}
local store                      -- gear.store.game handle, made on first use
local dialog                     -- {creature, kind, page, history} while a dialog is open
local pending                    -- {creature, until} the hero is walking to
local DEFAULT_TALK_RANGE = 140
local PANEL = 'dialog'

---------------------------------------------------------------------------------------------------------------
-- helpers

local function minutes_of(t)
    if type(t) == 'number' then return t % 1440 end
    if type(t) == 'string' then
        local h, m = t:match('^(%d+):(%d%d)$')
        if h and tonumber(h) < 24 and tonumber(m) < 60 then return tonumber(h) * 60 + tonumber(m) end
    end
    error("time must be minutes since midnight or 'HH:MM'", 3)
end

local function creature_id(c) return type(c) == 'table' and c.id or c end

local function data()
    if not store then store = gear.store.game('npc.json', {list = {}}) end
    local d = store.data
    if type(d) ~= 'table' then d = {list = {}}; store.data = d end
    if type(d.list) ~= 'table' then d.list = {} end
    return d, store
end

local function fire(list, e)
    for _, fn in ipairs(list) do
        local ok, err = pcall(fn, e)
        if not ok then gear.log('npc handler failed: ' .. tostring(err)) end
    end
end

local function resolve_type(t)
    if type(t) == 'string' then
        local id = gear.data.object_types:find(t)
        return id
    end
    return t
end

---------------------------------------------------------------------------------------------------------------
-- kinds

-- define(id, def) -> def. `id` is a string that is yours alone ('mymod.smith'); it is stored in the savegame's text ids, so
-- keep it stable. def:
--   type      the object type (id or name) whose model the NPC uses (required)
--   name      the name shown (a string or {en = ..., de = ...}); default: the type's name
--   level     the creature's level (default 1: the engine's plain create)
--   like      a creature whose faction, level and health the NPC takes (default: the hero's faction, so the NPC is friendly)
--   hostile   true: keep the faction of the type
--   dialog    {start = 'page', pages = {page = {text = string|fun(e), options = {...}}}}, see the docs
--   routine   {{time = 'HH:MM', x =, y =, layer =}, ...}: where the NPC stands from that time on
--   talk_range  how close the hero must be to talk (world units, default 140)
--   walk_radius when the hero is this close (to the NPC or its goal) the NPC walks to the next routine point; else it is put there
--   tint      a colour (0xRRGGBB) or {all = colour, parts = {[n] = colour}}: creature:set_tint; set again after a load
--   on_talk   fun(e): a dialog opened
function N.define(id, def)
    assert(type(id) == 'string' and id ~= '' and #id <= 100, 'id must be a string')
    assert(type(def) == 'table', 'def must be a table')
    local t = resolve_type(def.type)
    assert(type(t) == 'number', 'def.type must be an object type id or name that exists')
    local d = {}
    for k, v in pairs(def) do d[k] = v end
    d.id, d.type = id, t
    if d.name ~= nil then
        local text, err = gear.text.key(PREFIX .. id, d.name)
        assert(text, err)
        d.name_text = text
    end
    if d.routine then
        local r = {}
        for i, p in ipairs(d.routine) do
            assert(type(p.x) == 'number' and type(p.y) == 'number', 'routine point needs x and y')
            r[i] = {time = minutes_of(p.time), x = p.x, y = p.y, layer = p.layer}
        end
        table.sort(r, function(a, b) return a.time < b.time end)
        assert(#r > 0, 'routine needs at least one point')
        d.routine = r
    end
    d.talk_range = d.talk_range or DEFAULT_TALK_RANGE
    if not kinds[id] then order[#order + 1] = id end
    kinds[id] = d
    return d
end

function N.kind(id) return kinds[id] end
function N.kinds() local r = {}; for i, id in ipairs(order) do r[i] = id end; return r end

-- of(creature) -> kind id, definition | nil: which kind of this mod a creature is (by its name text).
function N.of(creature)
    local c = type(creature) == 'number' and gear.world.object(creature) or creature
    if not c then return nil end
    local key = c:name_key()
    if key and key:sub(1, #PREFIX) == PREFIX then
        local id = key:sub(#PREFIX + 1)
        if kinds[id] then return id, kinds[id] end
    end
    return nil
end

---------------------------------------------------------------------------------------------------------------
-- spawning and the list of NPCs

local looked = {}            -- creature id -> true: its tint was set this session (the tints end with the savegame)

local function apply_look(c, def, inst)
    local tint = (inst and inst.tint) or def.tint
    if tint ~= nil then
        local ok, why = c:set_tint(tint)
        if not ok then gear.log(('npc %s: tint: %s'):format(def.id, tostring(why))) end
    end
    looked[c.id] = true
end

-- spawn(kind, x, y, opts) -> creature | nil, message. opts: layer, like, level, tint (replaces the kind's), routine (replaces the kind's),
-- data (kept with the NPC: inst.data)
function N.spawn(kind, x, y, opts)
    opts = opts or {}
    local def = kinds[kind]
    if not def then return nil, 'no NPC kind ' .. tostring(kind) end
    local c, err = gear.world.spawn(def.type, x, y, {layer = opts.layer, like = opts.like or def.like})
    if not c then return nil, err end
    if def.name_text then
        local ok, why = c:set_name_text(def.name_text)
        if not ok then c:remove(); return nil, why end
    end
    if not (opts.like or def.like) and not def.hostile then
        -- a plain create has the faction of its type: take the hero's side, so the NPC is no enemy (its label is not red, a click does not attack)
        local hero = gear.world.hero()
        if hero then c:copy_allegiance(hero) end
    end
    local level = opts.level or def.level
    if level then c:set_level(level) end
    local d, h = data()
    local inst = {id = c.id, kind = kind, type = def.type, routine = opts.routine, data = opts.data, tint = opts.tint}
    apply_look(c, def, inst)
    d.list[#d.list + 1] = inst
    h:changed()
    return c
end

local function prune()
    local d, h = data()
    local keep, changed = {}, false
    for _, inst in ipairs(d.list) do
        local c = gear.world.object(inst.id)
        if c and c:is_creature() and c:type() == inst.type and not c:is_dead() then
            keep[#keep + 1] = inst
        else
            changed = true
        end
    end
    if changed then d.list = keep; h:changed() end
    return d.list
end

-- list(kind?) -> {{creature =, kind =, data =}, ...}: the live NPCs of this mod (of one kind).
function N.list(kind)
    local r = {}
    for _, inst in ipairs(prune()) do
        if (not kind or inst.kind == kind) and kinds[inst.kind] then
            r[#r + 1] = {creature = gear.world.object(inst.id), kind = inst.kind, data = inst.data}
        end
    end
    return r
end

local function inst_of(creature)
    local id = creature_id(creature)
    for _, inst in ipairs(data().list) do
        if inst.id == id then return inst end
    end
end

-- remove(creature): the NPC is removed from the world.
function N.remove(creature)
    local id = creature_id(creature)
    local d, h = data()
    for i, inst in ipairs(d.list) do
        if inst.id == id then table.remove(d.list, i); h:changed(); break end
    end
    local c = gear.world.object(id)
    if c then return c:remove() end
    return nil, 'no such creature'
end

---------------------------------------------------------------------------------------------------------------
-- events

-- on(name, fn) -> handle: 'talk' (e: npc, creature, hero), 'button' (e: npc, creature, page, option, ...), 'close', 'arrived'
-- (e: npc, creature, point: a routine point was reached).
function N.on(name, fn)
    assert(handlers[name], "event must be 'talk', 'button', 'close' or 'arrived'")
    assert(type(fn) == 'function', 'fn must be a function')
    local list = handlers[name]
    list[#list + 1] = fn
    return {name = name, fn = fn}
end

function N.off(handle)
    local list = handle and handlers[handle.name]
    if not list then return end
    for i, fn in ipairs(list) do
        if fn == handle.fn then table.remove(list, i); return end
    end
end

---------------------------------------------------------------------------------------------------------------
-- dialog

local function page_of(def, name) return def.dialog and def.dialog.pages and def.dialog.pages[name] end

local function event_for(extra)
    local e = {npc = dialog.kind, creature = dialog.creature, hero = gear.world.hero(), page = dialog.page}
    function e.close() dialog.closing = true end
    function e.go(page) dialog.next = page end
    function e.say(text) dialog.say = text end
    for k, v in pairs(extra or {}) do e[k] = v end
    return e
end

local function enter(page)
    local def = kinds[dialog.kind]
    if not page_of(def, page) then
        gear.log(('npc %s: no dialog page %s'):format(dialog.kind, tostring(page)))
        dialog = nil
        return
    end
    dialog.page = page
    dialog.say = nil
    dialog.history = dialog.history or {}
end

function N.close()
    if not dialog then return end
    local e = event_for()
    dialog = nil
    fire(handlers.close, e)
end

-- talk(creature, page?) -> true | nil, message: opens the NPC's dialog (the start page unless `page`).
function N.talk(creature, page)
    local kind, def = N.of(creature)
    if not kind then return nil, 'not an NPC of this mod' end
    if not def.dialog or not def.dialog.pages then return nil, 'the NPC has no dialog' end
    local c = type(creature) == 'number' and gear.world.object(creature) or creature
    dialog = {creature = c, kind = kind}
    enter(page or def.dialog.start or next(def.dialog.pages))
    if dialog then
        local e = event_for()
        if def.on_talk then local ok, err = pcall(def.on_talk, e); if not ok then gear.log('npc on_talk: ' .. tostring(err)) end end
        fire(handlers.talk, e)
    end
    return true
end

-- dialog_state() -> {npc, creature, page} | nil
function N.dialog_state()
    if not dialog then return nil end
    return {npc = dialog.kind, creature = dialog.creature, page = dialog.page}
end

local function press(opt, index)
    local def = kinds[dialog.kind]
    local e = event_for({option = opt.id or index, label = opt.label})
    if opt.on then
        local ok, err = pcall(opt.on, e)
        if not ok then gear.log('npc option: ' .. tostring(err)) end
    end
    fire(handlers.button, e)
    local target = dialog and dialog.next
    if dialog then
        if dialog.closing then N.close(); return end
        if target == nil then target = opt.go end
        if target ~= nil then
            dialog.next = nil
            enter(target)
        elseif opt.close then
            N.close()
        end
    end
    local _ = def
end

local function draw_dialog()
    if not dialog then return end
    local c = dialog.creature
    if not c or not c:exists() or c:is_dead() then N.close(); return end
    local hero = gear.world.hero()
    if hero and hero:distance(c) and hero:distance(c) > (kinds[dialog.kind].talk_range or DEFAULT_TALK_RANGE) * 3 then N.close(); return end
    local def = kinds[dialog.kind]
    local page = page_of(def, dialog.page)
    if not page then N.close(); return end
    local title = c:display_name() or dialog.kind
    local W = 420
    local text = dialog.say or page.text
    if type(text) == 'function' then
        local ok, res = pcall(text, event_for())
        text = ok and res or tostring(res)
    end
    text = tostring(text or '')
    local portrait = c:portrait()
    local _, text_h = ui.text_wrap(text, W - 16 - (portrait and 72 or 0), {})
    local count = 0
    for _, opt in ipairs(page.options or {}) do
        local visible = true
        if opt.when then
            local ok, res = pcall(opt.when, event_for())
            visible = ok and res ~= false
        end
        if visible then count = count + 1 end
    end
    local H = 70 + math.max(#(ui.text_wrap(text, W - 16 - (portrait and 72 or 0), {})) * 16, portrait and 72 or 0) + count * 28
    local _ = text_h
    local panel = ui.panel(PANEL, title, 300, 200, W, H, {layer = 'hud'})
    if not panel.open then N.close(); return end
    local x, y, w = panel.content_x + 8, panel.content_y + 6, panel.content_w - 16
    local tx = x
    if portrait then
        ui.image(portrait, x, y, 64, 64, {fit = 'contain'})
        tx = x + 72
    end
    local lines, th = ui.text_wrap(text, w - (tx - x), {})
    local ly = y
    for _, line in ipairs(lines) do
        ui.text(line, tx, ly, {color = 0xFFE8D8B0})
        ly = ly + 16
    end
    local by = math.max(ly, y + (portrait and 72 or 0)) + 12
    local _ = th
    local shown = {}
    for i, opt in ipairs(page.options or {}) do
        local visible = true
        if opt.when then
            local ok, res = pcall(opt.when, event_for())
            visible = ok and res ~= false
        end
        if visible then shown[#shown + 1] = {opt = opt, index = i} end
    end
    for _, s in ipairs(shown) do
        local label = s.opt.label
        if type(label) == 'function' then label = label(event_for()) end
        local on = true
        if s.opt.enabled then
            local ok, res = pcall(s.opt.enabled, event_for())
            on = ok and res ~= false
        end
        if ui.button('opt' .. s.index, x, by, w, 24, tostring(label or ''), {enabled = on}) and on then
            press(s.opt, s.index)
            return
        end
        by = by + 28
    end
end

---------------------------------------------------------------------------------------------------------------
-- clicking an NPC

local function npc_under_cursor(hero)
    local cur = gear.world.cursor(hero)
    local c = cur and cur.object
    if not c or not c:is_creature() then return nil end
    local kind, def = N.of(c)
    if kind and def.dialog then return c, kind, def end
end

local function click_frame(state)
    if state.mode ~= 'game' or state.escape_menu or state.message_box or state.exclusive or dialog then return end
    local hero = gear.world.hero()
    if not hero then return end
    if pending then
        local c = pending.creature
        local d = c:exists() and hero:distance(c)
        if not d or gear.time() > pending.untilt then
            pending = nil
        elseif d <= pending.range then
            pending = nil
            N.talk(c)
        end
    end
    -- (ui.over_ui() would be true from the second frame on: it counts the region this very function declares. A panel above the
    -- world layer takes the click before the region does.)
    local c, kind, def = npc_under_cursor(hero)
    if not c then return end
    local l, t, r, b = ui.screen()
    local reg = ui.region('click', l, t, r - l, b - t, {layer = 'world', nav = false})
    if reg.clicked and reg.hover then
        local d = hero:distance(c)
        if d and d <= def.talk_range then
            N.talk(c)
        else
            local x, y = c:position()
            local hx, hy = hero:position()
            local dx, dy = hx - x, hy - y
            local len = math.sqrt(dx * dx + dy * dy)
            if len > 1 then
                local k = math.max(def.talk_range - 40, 30) / len
                hero:order_move(x + dx * k, y + dy * k)
            end
            pending = {creature = c, range = def.talk_range, untilt = gear.time() + 12}
        end
    end
end

---------------------------------------------------------------------------------------------------------------
-- routines

-- The routine point an NPC should be at, by the game's clock: the last one whose time has come (before the first one of the day:
-- the last of the day before).
local function target_point(routine, now)
    local pick = routine[#routine]
    for _, p in ipairs(routine) do
        if p.time <= now then pick = p end
    end
    return pick
end

local ARRIVE = 45            -- world units: close enough to stand there

local function routine_tick()
    if not gear.world.can_change() then return end
    local now = gear.world.clock()
    local hero = gear.world.hero()
    if not now or not hero then return end
    local t = gear.time()
    for _, inst in ipairs(data().list) do
        local def = kinds[inst.kind]
        if def and not looked[inst.id] and (inst.tint ~= nil or def.tint ~= nil) then
            local c = gear.world.object(inst.id)
            if c and c:is_creature() then apply_look(c, def, inst) end
        end
        local routine = inst.routine or (def and def.routine)
        local c = routine and gear.world.object(inst.id)
        if c and c:is_creature() and not c:is_dead() and not (dialog and dialog.creature and dialog.creature.id == inst.id) then
            local p = target_point(routine, now)
            local x, y = c:position()
            local d = math.sqrt((x - p.x) ^ 2 + (y - p.y) ^ 2)
            local walk_radius = def.walk_radius or 900
            if d <= ARRIVE then
                if inst.walk then
                    inst.walk = nil
                    fire(handlers.arrived, {npc = inst.kind, creature = c, point = p})
                end
            else
                local near = (hero:distance(x, y) or 1e9) <= walk_radius or (hero:distance(p.x, p.y) or 1e9) <= walk_radius
                local w = inst.walk
                if not near then
                    gear.log(('npc %s: put at its point (hero far away)'):format(inst.kind))
                    c:teleport(p.x, p.y)
                    inst.walk = nil
                elseif not w or w.to ~= p then
                    local ok, why = c:order_move(p.x, p.y)
                    gear.log(('npc %s: walking %d units: %s'):format(inst.kind, d, ok and 'ok' or tostring(why)))
                    inst.walk = {to = p, since = t, last = t, x = x, y = y}
                else
                    -- walking: moved at all in the last 3 seconds? else blocked
                    if t - w.last >= 3 then
                        local moved = math.sqrt((x - w.x) ^ 2 + (y - w.y) ^ 2)
                        if moved < 8 then
                            gear.log(('npc %s: blocked, put at its point'):format(inst.kind))
                            c:teleport(p.x, p.y)
                            inst.walk = nil
                        else
                            w.last, w.x, w.y = t, x, y
                            c:order_move(p.x, p.y)
                        end
                    end
                    if inst.walk and t - w.since > 120 then
                        gear.log(('npc %s: walk took too long, put at its point'):format(inst.kind))
                        c:teleport(p.x, p.y)
                        inst.walk = nil
                    end
                end
            end
        end
    end
end

---------------------------------------------------------------------------------------------------------------

gear.on('frame', function(state)
    local ok, err = pcall(click_frame, ui.state())
    if not ok then gear.log('npc click: ' .. tostring(err)) end
end)
gear.on('draw', function(state)
    if state and state.mode ~= 'game' then dialog = nil; return end
    local ok, err = pcall(draw_dialog)
    if not ok then gear.log('npc dialog: ' .. tostring(err)); dialog = nil end
end)
gear.every(1, function()
    local ok, err = pcall(routine_tick)
    if not ok then gear.log('npc routine: ' .. tostring(err)) end
end)
gear.on('savegame.changed', function() dialog, pending, looked = nil, nil, {} end)

-- where(kind or creature) -> the routine point an NPC should be at now: x, y, layer.
function N.where(creature)
    local inst = inst_of(creature)
    local def = inst and kinds[inst.kind]
    local routine = inst and (inst.routine or (def and def.routine))
    local now = gear.world.clock()
    if not routine or not now then return nil end
    local p = target_point(routine, now)
    return p.x, p.y, p.layer
end

-- routine(creature, points | nil): gives one NPC its own routine (nil: the kind's again).
function N.routine(creature, points)
    local inst = inst_of(creature)
    if not inst then return nil, 'not an NPC of this mod' end
    if points then
        local r = {}
        for i, p in ipairs(points) do r[i] = {time = minutes_of(p.time), x = p.x, y = p.y, layer = p.layer} end
        table.sort(r, function(a, b) return a.time < b.time end)
        inst.routine = r
    else
        inst.routine = nil
    end
    inst.walk = nil
    local _, h = data()
    h:changed()
    return true
end

return N
