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
local handlers = {talk = {}, button = {}, close = {}, arrived = {}, hostile = {}, calm = {}}
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

-- A routine as the library keeps it: times in minutes, sorted.
local function normalize_routine(points)
    assert(type(points) == 'table' and #points > 0, 'a routine needs at least one point')
    local r = {}
    for i, p in ipairs(points) do
        assert(type(p.x) == 'number' and type(p.y) == 'number', 'routine point needs x and y')
        r[i] = {time = minutes_of(p.time), x = p.x, y = p.y, layer = p.layer}
    end
    table.sort(r, function(a, b) return a.time < b.time end)
    return r
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

---------------------------------------------------------------------------------------------------------------
-- kinds

-- define(id, def) -> def. `id` is a string that is yours alone ('mymod.smith'); it is stored in the savegame's text ids, so
-- keep it stable. def:
--   type      the object type (id or name) whose model the NPC uses (required; looked up when an NPC is spawned)
--   name      the name shown (a string or {en = ..., de = ...}); default: the type's name
--   level     the creature's level (default 1: the engine's plain create)
--   like      a creature whose faction, level and health the NPC takes (default: the hero's faction, so the NPC is friendly)
--   stance    'friendly' (default: the hero's side), 'neutral' (an enemy of nobody) or 'hostile' (an enemy of the hero from the start); npc.hostile
--             turns any NPC hostile later
--   hostile   true: the same as stance = 'hostile'
--   dialog    {start = 'page', pages = {page = {text = string|fun(e), options = {...}}}}, see the docs
--   routine   {{time = 'HH:MM', x =, y =, layer =}, ...}: where the NPC stands from that time on
--   talk_range  how close the hero must be to talk (world units, default 140)
--   walk_radius when the hero is this close (to the NPC or its goal) the NPC walks to the next routine point; else it is put there
--   texture   another texture for the model: a file of the mod ('skin.png', both sides a power of two), {pak = 'NAME.TGA'} (a texture of the game's
--             Texture.pak) or {all = t, parts = {[n] = t}} of those, for single meshes; the same texture layout as the model's own (creature:set_texture)
--   tint      a colour (0xRRGGBB) or {all = colour, parts = {[n] = colour}}: creature:set_tint; set again after a load
--   on_talk   fun(e): a dialog opened
function N.define(id, def)
    assert(type(id) == 'string' and id ~= '' and #id <= 100, 'id must be a string')
    assert(type(def) == 'table', 'def must be a table')
    -- (the type is looked up when an NPC is spawned: the game's object types are not loaded yet when the mods start in the menu)
    assert(type(def.type) == 'number' or type(def.type) == 'string', 'def.type must be an object type id or name')
    local d = {}
    for k, v in pairs(def) do d[k] = v end
    d.id = id
    if d.name ~= nil then
        local text, err = gear.text.key(PREFIX .. id, d.name)
        assert(text, err)
        d.name_text = text
    end
    if d.routine then d.routine = normalize_routine(d.routine) end
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

local textures = {}          -- file path -> its gear.ui.texture handle (kept: the creatures use the picture)

-- An image id for creature:set_texture: a file of the mod (PNG, TGA, JPG, DDS, both sides a power of two), {pak = 'NAME.TGA'} (the game's
-- Texture.pak) or an image id as it is.
local function texture_id(v)
    if type(v) == 'number' then return v end
    if type(v) == 'table' and v.pak then return ui.texture_image(v.pak), 'no texture ' .. tostring(v.pak) .. ' in the game' end
    if type(v) == 'string' then
        local t = textures[v]
        if not t then
            local err
            t, err = ui.texture.load(v)
            if not t then return nil, err end
            textures[v] = t
        end
        local function pow2(n) return n >= 1 and n == 2 ^ math.floor(math.log(n) / math.log(2) + 0.5) end
        if not pow2(t.width) or not pow2(t.height) then return nil, v .. ': both sides must be a power of two' end
        return t.picture, 'no picture'
    end
    return nil, 'a texture is a file name, {pak = name} or an image id'
end

local function texture_spec(spec)
    if type(spec) == 'table' and not spec.pak then
        local out = {parts = {}}
        local id, why
        if spec.all ~= nil then
            id, why = texture_id(spec.all)
            if not id then return nil, why end
            out.all = id
        end
        for part, v in pairs(spec.parts or {}) do
            id, why = texture_id(v)
            if not id then return nil, why end
            out.parts[part] = id
        end
        return out
    end
    local id, why = texture_id(spec)
    if not id then return nil, why end
    return {all = id}
end

local ordered_at = {}        -- creature id -> time of the last attack order
local looked_hostile = {}   -- creature id -> true: its boss modifiers were set this session (they end with the savegame)
local looked = {}            -- creature id -> true: its tint was set this session (the tints end with the savegame)

local function apply_look(c, def, inst)
    local tint = (inst and inst.tint) or def.tint
    if tint ~= nil then
        local ok, why = c:set_tint(tint)
        if not ok then gear.log(('npc %s: tint: %s'):format(def.id, tostring(why))) end
    end
    local texture = (inst and inst.texture) or def.texture
    if texture ~= nil then
        local spec, why = texture_spec(texture)
        local ok
        if spec then ok, why = c:set_texture(spec) end
        if not ok then gear.log(('npc %s: texture: %s'):format(def.id, tostring(why))) end
    end
    looked[c.id] = true
end

-- The faction that is an enemy of neither the hero nor the monsters about (found once by trying the factions on the creature), nil if none.
local neutral_cache
local function neutral_faction(c)
    if neutral_cache then return neutral_cache end
    local hero = gear.world.hero()
    if not hero then return nil end
    local own = c:allegiance()
    local x, y, layer = hero:position()
    local foes = {}
    for _, o in ipairs(gear.world.creatures_near(x, y, 900, {limit = 40})) do
        if o.id ~= hero.id and o.id ~= c.id and hero:is_enemy(o) then foes[#foes + 1] = o end
    end
    for _, f in ipairs({3, 4, 7, 0}) do
        c:set_allegiance{faction = f, aggressive = false}
        local ok = not c:is_enemy(hero) and not hero:is_enemy(c)
        for _, o in ipairs(foes) do
            if ok and (c:is_enemy(o) or o:is_enemy(c)) then ok = false end
        end
        if ok then neutral_cache = f break end
    end
    c:set_allegiance{faction = own}
    return neutral_cache
end

local function apply_stance(c, stance, faction0)
    local hero = gear.world.hero()
    if stance == 'hostile' then
        c:set_allegiance{aggressive = true}
    elseif stance == 'neutral' then
        local f = neutral_faction(c)
        if f then c:set_allegiance{faction = f, aggressive = false} elseif hero then c:copy_allegiance(hero) end
    elseif hero then
        c:copy_allegiance(hero)
    end
end

-- spawn(kind, x, y, opts) -> creature | nil, message. opts: layer, like, stance, level, tint and texture (replace the kind's), routine (replaces the kind's),
-- data (kept with the NPC: inst.data)
function N.spawn(kind, x, y, opts)
    opts = opts or {}
    local def = kinds[kind]
    if not def then return nil, 'no NPC kind ' .. tostring(kind) end
    local c, err = gear.world.spawn(def.type, x, y, {layer = opts.layer, like = opts.like or def.like})
    if not c then return nil, err end
    def.type_id = c:type()
    if def.name_text then
        local ok, why = c:set_name_text(def.name_text)
        if not ok then c:remove(); return nil, why end
    end
    -- A plain create has the faction of its type, which is often an enemy of the hero. The stance says what the NPC is: 'friendly' (the default:
    -- the hero's side), 'neutral' (an enemy of nobody) or 'hostile' (the type's own faction, aggressive). The faction the type gave is kept so
    -- that npc.hostile can restore it.
    local faction0, aggressive0 = c:allegiance()
    local stance = opts.stance or def.stance or (def.hostile and 'hostile') or 'friendly'
    if not (opts.like or def.like) then apply_stance(c, stance, faction0) end
    local level = opts.level or def.level
    if level then c:set_level(level) end
    local d, h = data()
    local inst = {id = c.id, kind = kind, type = c:type(), routine = opts.routine and normalize_routine(opts.routine), data = opts.data, tint = opts.tint, texture = opts.texture,
        stance = stance, faction0 = faction0, aggressive0 = aggressive0}
    if stance == 'hostile' then inst.hostile = {} end
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
    assert(handlers[name], "event must be 'talk', 'button', 'close', 'arrived', 'hostile' or 'calm'")
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
    function e.hostile(opts) dialog.turn = opts or {} end
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
    if dialog and opt.hostile and not dialog.turn then dialog.turn = type(opt.hostile) == 'table' and opt.hostile or {} end
    if dialog and dialog.turn then
        -- the option made the NPC an enemy: the dialog ends and the fight starts
        local c, turn = dialog.creature, dialog.turn
        N.close()
        N.hostile(c, turn)
        return
    end
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

-- The look of the game's own NPC dialog: a wide frame (the item tooltip's) near the top of the screen, the portrait at the left, the NPC's name
-- in yellow over its text, and the buttons along the bottom, centred in equal shares of the width; a button's text is green and yellow
-- under the cursor.
local FONT = 'game3'
local C_NAME, C_TEXT, C_BUTTON, C_HOVER = 0xFFFFFF7F, 0xFFFFFFFF, 0xFF1EE61E, 0xFFFFFF00

local function draw_dialog()
    if not dialog then return end
    local c = dialog.creature
    if not c or not c:exists() or c:is_dead() then N.close(); return end
    local hero = gear.world.hero()
    if hero and hero:distance(c) and hero:distance(c) > (kinds[dialog.kind].talk_range or DEFAULT_TALK_RANGE) * 3 then N.close(); return end
    local def = kinds[dialog.kind]
    local page = page_of(def, dialog.page)
    if not page then N.close(); return end
    local text = dialog.say or page.text
    if type(text) == 'function' then
        local ok, res = pcall(text, event_for())
        text = ok and res or tostring(res)
    end
    text = tostring(text or '')

    local shown = {}
    for i, opt in ipairs(page.options or {}) do
        local visible = true
        if opt.when then
            local ok, res = pcall(opt.when, event_for())
            visible = ok and res ~= false
        end
        if visible then shown[#shown + 1] = {opt = opt, index = i} end
    end
    if #shown == 0 then shown[1] = {opt = {label = 'OK', close = true}, index = 1} end

    local l, t, r = ui.screen()
    local W = math.min(r - l - 40, 980)
    local x, y = l + (r - l - W) / 2, t + 28
    local portrait = c:portrait()
    local text_x = x + (portrait and 128 or 28)
    local text_w = W - (text_x - x) - 28
    local lines = ui.text_wrap(text, text_w, {font = FONT})
    local _, line_h = ui.text_size('Mg', FONT)
    line_h = (line_h or 18) + 2
    local H = math.max(34 + (#lines + 1) * line_h + 50, portrait and 160 or 0)

    ui.native.frame(x, y, W, H, {layer = 'hud'})
    if portrait then ui.image(portrait, x + 12, y + 8, 108, 128, {fit = 'contain', layer = 'hud'}) end
    ui.text(c:display_name() or dialog.kind, text_x, y + 14, {font = FONT, color = C_NAME, layer = 'hud'})
    local ty = y + 14 + line_h + 2
    for _, line in ipairs(lines) do
        ui.text(line, text_x, ty, {font = FONT, color = C_TEXT, layer = 'hud'})
        ty = ty + line_h
    end
    -- the buttons: the width divided by their number, each centred in its share
    local n = #shown
    local by = y + H - 44
    for k, s in ipairs(shown) do
        local label = s.opt.label
        if type(label) == 'function' then label = label(event_for()) end
        label = tostring(label or '')
        local on = true
        if s.opt.enabled then
            local ok, res = pcall(s.opt.enabled, event_for())
            on = ok and res ~= false
        end
        local tw, th = ui.text_size(label, FONT)
        tw, th = tw or 40, th or 18
        local cx = x + W * (k - 0.5) / n
        local rx, ry, rw, rh = cx - tw / 2 - 8, by - 4, tw + 16, th + 8
        local reg = ui.region('opt' .. s.index, rx, ry, rw, rh, {layer = 'hud'})
        local color = not on and 0xFF707070 or (reg.hover and C_HOVER or C_BUTTON)
        ui.text(label, cx - tw / 2, by, {font = FONT, color = color, layer = 'hud'})
        if reg.clicked and on then
            press(s.opt, s.index)
            return
        end
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

-- Walks the hero toward the NPC: to the point `range - 40` units from it on the hero's side. The engine ends the hero's walk order after
-- the next waypoint unless the left button is held, so the order is sent again every few tenths of a second until the hero is there.
local function walk_hero_to(hero, c, range)
    local x, y = c:position()
    local hx, hy = hero:position()
    if not x or not hx then return end
    local dx, dy = hx - x, hy - y
    local len = math.sqrt(dx * dx + dy * dy)
    if len > 1 then
        local k = math.max(range - 40, 30) / len
        hero:order_move(x + dx * k, y + dy * k)
    end
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
        elseif gear.time() - pending.ordered >= 0.3 then
            pending.ordered = gear.time()
            walk_hero_to(hero, c, pending.range)
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
            walk_hero_to(hero, c, def.talk_range)
            pending = {creature = c, range = def.talk_range, untilt = gear.time() + 20, ordered = gear.time()}
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
        if def and not looked[inst.id] and (inst.tint ~= nil or def.tint ~= nil or inst.texture ~= nil or def.texture ~= nil) then
            local c = gear.world.object(inst.id)
            if c and c:is_creature() then apply_look(c, def, inst) end
        end
        if inst.hostile and def then
            local hc = gear.world.object(inst.id)
            if hc and hc:is_creature() and not hc:is_dead() then
                local h = inst.hostile
                if not looked_hostile[inst.id] then
                    looked_hostile[inst.id] = true
                    local mods = {}
                    for _, k in ipairs({'health', 'damage', 'taken', 'deflect', 'reflect', 'regen'}) do mods[k] = h[k] end
                    if next(mods) then
                        mods.base_hp = h.base_hp
                        hc:set_modifiers(mods)
                    end
                    if h.tint ~= nil then hc:set_tint(h.tint) end
                end
                local d = hero:distance(hc)
                if d and d <= (h.aggro or 1500) and not hero:is_dead() and t - (ordered_at[inst.id] or -100) >= 1.5 then
                    ordered_at[inst.id] = t
                    hc:order_attack(hero)
                end
            end
        else
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
                    -- walking: moved at all in the last 1.5 seconds? The engine often drops the first order (the NPC's own AI is busy), so
                    -- the order is sent again; only after three orders that got it nowhere is it blocked, and put at its point.
                    if t - w.last >= 1.5 then
                        local moved = math.sqrt((x - w.x) ^ 2 + (y - w.y) ^ 2)
                        if moved < 8 then
                            w.tries = (w.tries or 0) + 1
                            if w.tries >= 3 then
                                gear.log(('npc %s: blocked, put at its point'):format(inst.kind))
                                c:teleport(p.x, p.y)
                                inst.walk = nil
                            else
                                w.last = t
                                c:order_move(p.x, p.y)
                            end
                        else
                            w.last, w.x, w.y, w.tries = t, x, y, 0
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

-- hostile(creature, opts) -> true | nil, message: the NPC turns into an enemy of the hero: the faction the object type gave it (an enemy of the
-- hero), aggressive, and it attacks the hero (a created creature does not notice heroes on its own, so the library orders it to). opts:
--   level       the creature's level (creature:set_level: the game derives health, damage and attributes from the type and the level)
--   health, damage, taken, deflect, reflect, regen   boss modifiers (creature:set_modifiers; health is a multiple of the maximum)
--   weapon      an item type (id or name) the NPC wields, or {main = type, off = type} (a shield or second weapon)
--   tint        a colour for the model (creature:set_tint)
--   aggro       how far away from the hero it attacks (world units, default 1500)
function N.hostile(creature, opts)
    opts = opts or {}
    local inst = inst_of(creature)
    if not inst then return nil, 'not an NPC of this mod' end
    local c = gear.world.object(inst.id)
    local hero = gear.world.hero()
    if not c or not c:is_creature() or not hero then return nil, 'no such creature' end
    if inst.hostile and inst.hostile.done then return true end
    -- the faction of the type, if that is an enemy of the hero; else the first enemy faction
    local ok = false
    for _, f in ipairs({inst.faction0 or 12, 12, 13, 2, 5}) do
        c:set_allegiance{faction = f, aggressive = true}
        if c:is_enemy(hero) then ok = true break end
    end
    if not ok then return nil, 'no faction hostile to the hero' end
    local h = {done = true, aggro = opts.aggro, level = opts.level, tint = opts.tint, weapon = opts.weapon}
    inst.hostile = h
    if opts.level then c:set_level(opts.level) end
    h.base_hp = c:max_hp()
    local mods = {}
    for _, k in ipairs({'health', 'damage', 'taken', 'deflect', 'reflect', 'regen'}) do
        if opts[k] ~= nil then mods[k] = opts[k]; h[k] = opts[k] end
    end
    if next(mods) then
        local good, why = c:set_modifiers(mods)
        if not good then gear.log(('npc %s: modifiers: %s'):format(inst.kind, tostring(why))) end
        looked_hostile[inst.id] = true
    end
    if opts.weapon ~= nil then
        local w = opts.weapon
        if type(w) ~= 'table' then w = {main = w} end
        local x, y, layer = c:position()
        for slot, typ in pairs({[12] = w.main, [13] = w.off}) do
            local item, why = gear.world.spawn(typ, x, y, {layer = layer})
            if item then
                local carried, why2 = c:carry(slot, item)
                if not carried then gear.log(('npc %s: weapon: %s'):format(inst.kind, tostring(why2))) item:remove() end
            else
                gear.log(('npc %s: weapon: %s'):format(inst.kind, tostring(why)))
            end
        end
    end
    if opts.tint ~= nil then c:set_tint(opts.tint) end
    c:order_attack(hero)
    local _, store_h = data()
    store_h:changed()
    fire(handlers.hostile, {npc = inst.kind, creature = c, hero = hero})
    return true
end

-- calm(creature): the NPC is as it was spawned again (its stance), its modifiers are taken off and it stops fighting.
function N.calm(creature)
    local inst = inst_of(creature)
    if not inst then return nil, 'not an NPC of this mod' end
    local c = gear.world.object(inst.id)
    if not c then return nil, 'no such creature' end
    c:set_modifiers(nil)
    c:order_stop()
    apply_stance(c, inst.stance == 'hostile' and 'friendly' or inst.stance or 'friendly', inst.faction0)
    inst.hostile = nil
    looked_hostile[inst.id] = nil
    local _, h = data()
    h:changed()
    fire(handlers.calm, {npc = inst.kind, creature = c})
    return true
end

-- is_hostile(creature) -> boolean
function N.is_hostile(creature)
    local inst = inst_of(creature)
    return inst ~= nil and inst.hostile ~= nil
end

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
        inst.routine = normalize_routine(points)
    else
        inst.routine = nil
    end
    inst.walk = nil
    local _, h = data()
    h:changed()
    return true
end

return N
