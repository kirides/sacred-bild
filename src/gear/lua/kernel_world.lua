---------------------------------------------------------------------------------------------------------------
-- The world (gear.world). Objects are referred to by id; a creature object is a small table {id = id} with methods
-- that read the game each time (an id whose object is gone reads as nil).
---------------------------------------------------------------------------------------------------------------
local TILE = 53.66563   -- world units per tile
local world = {TILE = TILE}
local labels = {}       -- object id -> {text, mod id}; per session

local creature_mt = {}
creature_mt.__index = creature_mt
creature_mt.__metatable = false
creature_mt.__eq = function(a, b) return a.id == b.id end
creature_mt.__tostring = function(c) return 'creature ' .. c.id end

local function wrap(id)
    if not id then return nil end
    return setmetatable({id = id}, creature_mt)
end

local function info(c) return native.object(c.id) end
function creature_mt:exists() return info(self) ~= nil end
function creature_mt:info() return info(self) end
function creature_mt:is_creature() local o = info(self); return o ~= nil and o.creature end
function creature_mt:type() local o = info(self); return o and o.type end
function creature_mt:type_name() local o = info(self); return o and data.object_types:name(o.type) end
-- display_name(): the name the game shows: the creature's own name text (creature:set_name_text, a hero's name) or its type's name.
function creature_mt:display_name()
    local o = info(self)
    if not o then return nil end
    local text = native.name_text(self.id)
    if text and text ~= 0 then
        local own = native.text_get(text)
        if own and own ~= '' then return own end
    end
    return data.object_types:label(o.type)
end
-- name_text() -> the text id of the creature's own name (+0x3C), 0 if it shows its type's name, nil if there is no such creature.
function creature_mt:name_text() return native.name_text(self.id) end
-- set_name_text(id): the name the game shows over the creature (0: its type's name again); `id` is a text id (gear.text.key, gear.text.add).
function creature_mt:set_name_text(id)
    assert(type(id) == 'number', 'id must be a text id')
    return native.set_name_text(self.id, id)
end
-- name_key() -> the gear.text.key key of the creature's own name text, if a mod registered it this session.
function creature_mt:name_key()
    local text = native.name_text(self.id)
    return text and text ~= 0 and native.text_key_of(text) or nil
end
function creature_mt:position() local o = info(self); if o then return o.x, o.y, o.layer end end
function creature_mt:sector() local o = info(self); return o and o.sector end
function creature_mt:hp() local o = info(self); return o and o.hp end
function creature_mt:max_hp() local o = info(self); return o and o.max_hp end
function creature_mt:is_dead() local o = info(self); return not o or not o.creature or o.hp <= 0 end
-- in_combat(): the engine's own "enemies about" state of the creature (bit 0x8000 of its state word): what stops ReBorn's teleportation potion
function creature_mt:in_combat() local o = info(self); return o ~= nil and o.in_combat == true end
-- casting(): the id of the art or spell the creature has in flight (its word at +0x120), nil when none
function creature_mt:casting() local o = info(self); local a = o and o.casting; return a and a ~= 0 and a or nil end
-- apply_effect(effect, value) -> true | nil, message: sends the creature the message a drunk potion sends its drinker (see GearCreature)
function creature_mt:apply_effect(effect, value)
    assert(type(effect) == 'number' and type(value) == 'number', 'effect and value must be numbers')
    return native.creature_effect(self.id, math.floor(effect), math.floor(value))
end
function creature_mt:gold() local o = info(self); return o and o.gold end
function creature_mt:level() local o = info(self); return o and o.level end
function creature_mt:experience() local o = info(self); return o and o.exp end
function creature_mt:set_hp(hp)
    assert(type(hp) == 'number', 'hp must be a number')
    return native.set_health(self.id, math.floor(hp))
end
-- allegiance() -> faction (0..15), aggressive: the creature's relation class and whether its behavior flags say aggressive (bit 0).
function creature_mt:allegiance()
    local faction, behavior = native.allegiance(self.id)
    if not faction then return nil end
    return faction, behavior % 2 == 1
end
-- set_allegiance{faction = 0..15, aggressive = bool}: a field left out stays (single player; not heroes).
function creature_mt:set_allegiance(spec)
    assert(type(spec) == 'table', 'the allegiance is a table {faction =, aggressive =}')
    local faction, behavior = native.allegiance(self.id)
    if not faction then return nil, 'no such creature' end
    if spec.faction ~= nil then
        assert(type(spec.faction) == 'number' and spec.faction >= 0 and spec.faction <= 15, 'faction is 0 to 15')
        faction = math.floor(spec.faction)
    end
    if spec.aggressive ~= nil then
        behavior = behavior - behavior % 2 + (spec.aggressive and 1 or 0)
    end
    return native.set_allegiance(self.id, faction, behavior)
end
-- flags(word) -> integer | nil: a creature's raw flag word, `word` one of 'object' (also for items), 'behavior', 'state', 'rank'
-- (see gear.world.FLAGS). set_flags(word, set, clear) -> true | nil, message: sets the bits of `set`, then clears those of `clear`
-- (single player). The object manager's own bits of the object word (0x2 and 0x10) are never changed.
world._flag_words = {object = 0, behavior = 1, state = 2, rank = 3}
function creature_mt:flags(word)
    local which = world._flag_words[word]
    assert(which, "word is 'object', 'behavior', 'state' or 'rank'")
    return native.flag_word(self.id, which)
end
function creature_mt:set_flags(word, set, clear)
    local which = world._flag_words[word]
    assert(which, "word is 'object', 'behavior', 'state' or 'rank'")
    assert(type(set) == 'number' and (clear == nil or type(clear) == 'number'), 'set_flags(word, set, clear): masks are numbers')
    local value = native.flag_word(self.id, which)
    if not value then return nil, 'no such creature' end
    value = bit.bor(value, set)
    if clear then value = bit.band(value, bit.bnot(clear)) end
    return native.set_flag_word(self.id, which, value % 4294967296)
end
-- carry(slot, item): puts an item object (made with gear.world.spawn, lying in the world) into equipment slot 0..18 of a creature that is no hero:
-- it leaves the world and the creature carries it there, as the creatures the game places do (single player).
function creature_mt:carry(slot, item)
    local id = type(item) == 'table' and item.id or item
    assert(type(slot) == 'number' and type(id) == 'number', 'carry(slot, item)')
    return native.carry(self.id, slot, id)
end
-- copy_allegiance(source): the faction and aggression of another creature (single player).
function creature_mt:copy_allegiance(source)
    local id = type(source) == 'table' and source.id or source
    assert(type(id) == 'number', 'source must be a creature or its id')
    return native.copy_allegiance(self.id, id)
end
-- copy_stats(source): the level, stats and health share of another creature (single player, not heroes).
function creature_mt:copy_stats(source)
    local id = type(source) == 'table' and source.id or source
    assert(type(id) == 'number', 'source must be a creature or its id')
    return native.copy_stats(self.id, id)
end
-- set_level(level): the level of a creature (not a hero) through the engine's own level function (single player).
function creature_mt:set_level(level)
    assert(type(level) == 'number', 'level must be a number')
    return native.set_level(self.id, math.floor(level))
end
-- set_pet{scale = 1.3, untargetable = true}: the model is drawn `scale` times as large and, if untargetable, cannot be picked in the
-- world (no click, hover cursor or controller aim). nil or {} clears it. Needs SacredBild; single player.
function creature_mt:set_pet(opts)
    if opts ~= nil and type(opts) ~= 'table' then error('the pet options are a table or nil', 2) end
    opts = opts or {}
    local scale = opts.scale == nil and 1 or opts.scale
    assert(type(scale) == 'number' and scale >= 0.1 and scale <= 4, 'scale must be a number from 0.1 to 4')
    return native.set_pet(self.id, scale, opts.untargetable == true)
end
-- set_tint(spec): the model is drawn with a colour multiplied over it. spec: a colour (0xAARRGGBB; a number below 0x1000000 is taken as
-- 0xRRGGBB, opaque) for the whole model, or {all = colour, parts = {[1] = colour, ...}} (the meshes of the model in the order the game
-- draws them: a character is a few meshes, e.g. body, cloth). nil clears it. Needs SacredBild; single player; ends with the savegame.
function creature_mt:set_tint(spec)
    local function argb(c)
        if c == nil or c == false then return 0 end
        assert(type(c) == 'number' and c >= 0 and c <= 0xFFFFFFFF, 'a colour is a number 0xAARRGGBB')
        if c < 0x1000000 then c = c + 0xFF000000 end
        return c
    end
    if spec == nil then
        for part = 0, 8 do
            local ok, err = native.set_tint(self.id, part, 0)
            if not ok then return nil, err end
        end
        return true
    end
    if type(spec) == 'number' then spec = {all = spec} end
    assert(type(spec) == 'table', 'the tint is a colour, a table or nil')
    local parts = spec.parts or {}
    local ok, err = native.set_tint(self.id, 0, argb(spec.all))
    if not ok then return nil, err end
    for part = 1, 8 do
        ok, err = native.set_tint(self.id, part, argb(parts[part]))
        if not ok then return nil, err end
    end
    return true
end
-- set_texture(spec): the model is drawn with other textures: spec is an image id (gear.ui.texture_image(name) for a Texture.pak texture, or the
-- `picture` of a gear.ui.texture.load handle for an image file of the mod) for the whole model, or {all = id, parts = {[n] = id}} for single
-- meshes (in the order the game draws them, as set_tint's). The texture takes the place of the mesh's own one, so it must fit its texture
-- coordinates (the same layout; a file's sides must be powers of two). nil clears it. Needs SacredBild; single player; ends with the savegame.
function creature_mt:set_texture(spec)
    if spec == nil then
        for part = 0, 8 do
            local ok, err = native.set_texture(self.id, part, 0)
            if not ok then return nil, err end
        end
        return true
    end
    if type(spec) == 'number' then spec = {all = spec} end
    assert(type(spec) == 'table', 'the texture is an image id, a table or nil')
    local parts = spec.parts or {}
    local ok, err = native.set_texture(self.id, 0, spec.all or 0)
    if not ok then return nil, err end
    for part = 1, 8 do
        ok, err = native.set_texture(self.id, part, parts[part] or 0)
        if not ok then return nil, err end
    end
    return true
end
function creature_mt:set_gold(gold)
    assert(type(gold) == 'number', 'gold must be a number')
    return native.set_gold(self.id, math.min(math.max(0, math.floor(gold)), 2147483646))
end
function creature_mt:add_gold(amount)
    assert(type(amount) == 'number', 'amount must be a number')
    local o = info(self)
    if not o or not o.gold then return nil, 'no such hero' end
    return native.set_gold(self.id, math.min(math.max(0, o.gold + math.floor(amount)), 2147483646))
end
function creature_mt:remove()
    return native.remove(self.id)
end
function creature_mt:teleport(x, y, layer)
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    local o = info(self)
    return native.teleport(self.id, math.floor(x), math.floor(y), layer or (o and o.layer) or 0)
end
---------------------------------------------------------------------------------------------------------------
-- Creature modifiers (creature:set_modifiers, the "world boss" building block). Kept per session in kernel.boss, by
-- object id: the engine knows nothing of them. The health is written into the creature (its maximum and current
-- value), the rest is applied by the `damage` hook (the damage the creature deals and takes, deflected hits,
-- reflected damage) and a slow tick (regeneration, forgetting creatures that are gone). Gear writes nothing of it
-- into a savegame.
---------------------------------------------------------------------------------------------------------------
kernel.boss = {mods = {}, count = 0, next_poll = 0}

function kernel.boss.number(v, name, lo, hi)
    if v == nil then return nil end
    assert(type(v) == 'number' and v == v, 'modifier ' .. name .. ' must be a number')
    return math.min(math.max(v, lo), hi)
end

-- Whether the damage hook is needed: some mod handles `damage`, or a creature has modifiers.
function kernel.boss.sync()
    local list = handlers['damage']
    local hits = handlers['ability.hit']
    native.damage_active(kernel.boss.count > 0 or (list ~= nil and #list > 0) or (hits ~= nil and #hits > 0))
end

function kernel.boss.forget(id)
    local e = kernel.boss.mods[id]
    if not e then return end
    kernel.boss.mods[id] = nil
    kernel.boss.count = kernel.boss.count - 1
    kernel.boss.sync()
end

-- All modifiers are forgotten (the game ended or another savegame was loaded: the object ids mean other creatures now).
function kernel.boss.reset()
    kernel.boss.mods = {}
    kernel.boss.count = 0
    kernel.boss.sync()
end

-- The entry of a live creature with modifiers (nil when it has none, is gone, or the id now names another creature).
function kernel.boss.entry(id)
    local e = kernel.boss.mods[id]
    if not e then return nil end
    local o = native.object(id)
    if not o or not o.creature or o.type ~= e.type then
        kernel.boss.forget(id)
        return nil
    end
    return e, o
end

-- The engine now and then recomputes a creature's maximum health from its own data (seen about half a minute into a fight): the boss
-- maximum is written again, with the share of health the creature had.
function kernel.boss.fix(id, e, o)
    if not e.want then return end
    if o.max_hp ~= e.want then
        e.resets = e.resets + 1
        native.set_max_health(id, e.want, math.max(1, math.floor((e.share or 1) * e.want)))
    elseif o.max_hp > 0 then
        e.share = o.hp / o.max_hp
    end
end

-- The hook: the damage after the mods' own `damage` handlers; returns the final damage.
function kernel.boss.hit(damage, victim, attacker)
    local dealt = attacker ~= 0 and kernel.boss.entry(attacker)
    if dealt and dealt.mods.damage then
        damage = damage * dealt.mods.damage
        dealt.stats.dealt = dealt.stats.dealt + 1
    end
    local taken, vo = nil, nil
    if victim ~= 0 then taken, vo = kernel.boss.entry(victim) end
    if taken then
        kernel.boss.fix(victim, taken, vo)
        local m, st = taken.mods, taken.stats
        st.hits = st.hits + 1
        if m.deflect and m.deflect > 0 and math.random() < m.deflect then
            st.deflected = st.deflected + 1
            return 0
        end
        if m.taken then damage = damage * m.taken end
        damage = math.floor(damage)
        st.damage = st.damage + damage
        if m.reflect and m.reflect > 0 and damage > 0 and attacker ~= 0 and attacker ~= victim then
            local ao = native.object(attacker)
            if ao and ao.creature and ao.hp and ao.hp > 1 then
                local back = math.floor(damage * m.reflect)
                if back > 0 then
                    -- as `set_hp`: the reflection never kills (the attacker keeps at least 1 hit point)
                    native.set_health(attacker, ao.hp - back)
                    st.reflected = st.reflected + math.min(back, ao.hp - 1)
                end
            end
        end
    end
    return math.floor(damage)
end

-- Slow tick: regeneration and forgetting the creatures that are gone (twice a second).
function kernel.boss.poll()
    local b = kernel.boss
    if b.count == 0 then return end
    local now = native.time()
    if now < b.next_poll then return end
    local elapsed = b.last and (now - b.last) or 0.5
    b.last = now
    b.next_poll = now + 0.5
    local ids = {}
    for id in pairs(b.mods) do ids[#ids + 1] = id end
    for _, id in ipairs(ids) do
        local e, o = b.entry(id)
        if e then
            b.fix(id, e, o)
            if e.mods.regen and e.mods.regen > 0 and o.hp > 0 and o.hp < o.max_hp then
                native.set_health(id, math.min(o.max_hp, o.hp + math.max(1, math.floor(o.max_hp * e.mods.regen * elapsed))))
            end
        end
    end
end

-- Sets the creature's modifiers (replacing the previous ones); see GearModifiers. An empty table or nil clears them.
function creature_mt:set_modifiers(mods)
    local b = kernel.boss
    if mods ~= nil and type(mods) ~= 'table' then error('the modifiers are a table or nil', 2) end
    mods = mods or {}
    local m = {
        health = kernel.boss.number(mods.health, 'health', 0.01, 100000),
        max_hp = kernel.boss.number(mods.max_hp, 'max_hp', 1, 1e9),
        damage = kernel.boss.number(mods.damage, 'damage', 0, 1000),
        taken = kernel.boss.number(mods.taken, 'taken', 0, 1000),
        reflect = kernel.boss.number(mods.reflect, 'reflect', 0, 10),
        deflect = kernel.boss.number(mods.deflect, 'deflect', 0, 1),
        regen = kernel.boss.number(mods.regen, 'regen', 0, 1),
        base_hp = kernel.boss.number(mods.base_hp, 'base_hp', 1, 1e9),
    }
    local o = info(self)
    if not o or not o.creature then return nil, 'no such creature' end
    if self.id >= 1 and self.id <= 16 then return nil, 'heroes cannot have modifiers' end
    if not native.world_changes() then return nil, 'creature modifiers work only in single player' end
    local old = b.mods[self.id]
    if old and old.type ~= o.type then old = nil end
    if next(m) == nil or (next(m) == 'base_hp' and next(m, 'base_hp') == nil) then
        -- cleared: the health goes back to the creature's own maximum, keeping its share
        if old and old.base_max then native.set_max_health(self.id, old.base_max) end
        if old then b.forget(self.id) end
        return true
    end
    local base = old and old.base_max or m.base_hp or o.max_hp
    local want = m.max_hp or (m.health and math.max(1, math.floor(base * m.health + 0.5))) or base
    if want ~= o.max_hp then
        local ok, err = native.set_max_health(self.id, want)
        if not ok then return nil, err end
    end
    if not old then b.count = b.count + 1 end
    b.mods[self.id] = {
        type = o.type, base_max = base, mods = m, resets = old and old.resets or 0,
        want = (want ~= base) and want or nil, share = nil,
        stats = old and old.stats or {hits = 0, deflected = 0, damage = 0, reflected = 0, dealt = 0},
    }
    b.sync()
    return true
end

-- The modifiers set on the creature (a copy), nil if it has none.
function creature_mt:modifiers()
    local e = kernel.boss.entry(self.id)
    if not e then return nil end
    local c = {}
    for k, v in pairs(e.mods) do c[k] = v end
    return c
end

-- What the modifiers did so far (hits taken, deflected, damage taken after the changes, damage reflected, hits dealt).
function creature_mt:modifier_stats()
    local e = kernel.boss.entry(self.id)
    if not e then return nil end
    local c = {base_max_hp = e.base_max, max_hp_resets = e.resets}
    for k, v in pairs(e.stats) do c[k] = v end
    return c
end


-- Distance in world units to another creature or to (x, y).
function creature_mt:distance(x, y)
    local ax, ay = self:position()
    if not ax then return nil end
    if type(x) == 'table' then x, y = x:position() end
    if not x then return nil end
    return math.sqrt((ax - x) ^ 2 + (ay - y) ^ 2)
end
-- A label Sacred Gear keeps for the object this session ("<viewer>'s Goblin"); mods read it back with :label().
-- (Shown in the game in a later milestone.)
function creature_mt:label()
    local l = labels[self.id]
    return l and l.text
end

function world.hero() return wrap(native.hero()) end
function world.object(id)
    assert(type(id) == 'number', 'id must be a number')
    if native.object(id) then return wrap(id) end
end
function world.can_change() return native.world_changes() end

-- world.open_teleporter(sub) -> true | nil, message: opens the game's list of the portals the hero knows, as a portal does
function world.open_teleporter(sub) return native.open_teleporter(sub or 0) end

-- world.show_text(text_id, color?) -> true | nil, message: a line of the game's text `text_id` in the HUD (as the game's own refusals show them)
function world.show_text(text_id, color)
    assert(type(text_id) == 'number', 'text_id must be a number')
    return native.show_text(text_id, color or 0)
end

-- world.portals(mask?) -> mask | nil, message: the portals the local hero knows (one bit per portal); with `mask` they are set (single player)
function world.portals(mask) return native.portals(mask) end
-- The creatures that have modifiers (creature:set_modifiers) now.
function world.modified()
    local out = {}
    for id in pairs(kernel.boss.mods) do
        if kernel.boss.entry(id) then out[#out + 1] = wrap(id) end
    end
    table.sort(out, function(a, c) return a.id < c.id end)
    return out
end

-- Creatures within `radius` world units of (x, y) on `layer` (default 0), nearest first: at most opts.limit (64).
function world.creatures_near(x, y, radius, opts)
    assert(type(x) == 'number' and type(y) == 'number' and type(radius) == 'number', 'x, y, radius must be numbers')
    opts = opts or {}
    local out = {}
    for i, id in ipairs(native.creatures_near(x, y, opts.layer or 0, radius, opts.limit or 64)) do
        out[i] = wrap(id)
    end
    return out
end

-- Hero classes: the hero's type (1..9) and the bit an item's class mask has for it.
local CLASSES = {
    {id = 1, name = 'seraphim', bit = 0x1}, {id = 2, name = 'gladiator', bit = 0x2},
    {id = 3, name = 'battlemage', bit = 0x4}, {id = 4, name = 'darkelf', bit = 0x8},
    {id = 5, name = 'woodelf', bit = 0x10}, {id = 6, name = 'vampiress', bit = 0x20},
    {id = 8, name = 'dwarf', bit = 0x400000}, {id = 9, name = 'daemon', bit = 0x800000},
}
local CLASS_INFO = {by_id = {}, all = 0xC0003F}   -- one local: the main chunk is at Lua's 200-locals limit
for _, c in ipairs(CLASSES) do CLASS_INFO.by_id[c.id] = c end
world.CLASSES = CLASSES

-- Creature flag words, by name: the bits of three words every creature has. Each name was read from the game's own tests of the bit
-- (Sacred.exe: the creature render 0x599910, the cursor 0x611E70, the talk query 0x549920, the AI state 0x4EE030, the tag handlers
-- of CreateNPC 0x482510 and Dialog 0x48F9E0). `object` is the word `creature:info().flags` returns (object +0x14), `behavior` the
-- second value of `creature:allegiance()` (+0x1F4, what a creature does by itself), `state` the creature's state word (+0x200).
world.FLAGS = {
    object = {
        NOT_IN_WORLD = 0x2,          -- carried or being picked up (an item), not lying in the world
        REMOVED = 0x10,              -- freed or parked: the object is gone
        DIALOGUE_ICON = 0x80000,     -- the "!?" icon over its head, set by every dialogue assignment, cleared when its dialogue goes (+0x245 < 1)
        THROWN = 0x100000,           -- in a throw's flight
        MADE_BY_ENTER = 0x80000000,  -- made by a sector's Enter function (the fills' creatures among them)
    },
    behavior = {
        AGGRESSIVE = 0x1,            -- hostile and awake: attacks (what `allegiance()` reports as aggressive)
        FOLLOWER = 0x4,              -- follows a partner (the scripts' companions, CreateNPC tags 0x37 and 0x71)
        TOWNSPERSON = 0x8,           -- the townsfolk's mode (CreateNPC tag 0x2B; the bit 0x8000 runs as this one)
        ANIMAL_10 = 0x10,            -- the four animal modes (CreateNPC tags 0x2E, 0x30, 0x2F/0x31, 0x32): they move only while standing
        ANIMAL_20 = 0x20,            -- (the loose farm animals' mode; waters' fish take it too)
        ANIMAL_40 = 0x40,
        ANIMAL_80 = 0x80,
        HIRED = 0x100,               -- a hired follower fighting at its master's side
        TOWNSPERSON_400 = 0x400,     -- the townsfolk's second mode (CreateNPC tag 0x42)
        STANDS_800 = 0x800,          -- stands (tag 0x43)
        STANDS_1000 = 0x1000,        -- stands (tag 0x44)
        GUARD = 0x4000,              -- the guards' mode (tag 0x46)
        TOWNSPERSON_8000 = 0x8000,   -- tag 0x4C, handled as TOWNSPERSON
        SLIDE = 0x20000,             -- a slide
        NO_TALK = 0x40000,           -- gets no generic dialogue and no speech-bubble cursor (the AI also lets it look among the creatures about it for a fight)
        BOSS = 0x80000,              -- a boss: its ring is drawn under it
        FLYING = 0x200000,           -- flies (the crows, hornets)
        SIGHT = 0x400000,            -- runs its sight function (`See:`) once the hero comes in view
        JUGGLER = 0x10000000,        -- the jugglers' mode
        EFFECT_32_MARK = 0x40000000, -- set while effect 32 holds (no stat change); the creature render draws an extra marker over a creature that has it
        -- A creature with any of these bits is one that the game lets talk, show a doing's icon (Alt) and wander in the AI's behaviours.
        TOWNSFOLK_MASK = 0x1006DCF8,
    },
    rank = {
        CHAMPION = 0x400,            -- champion-levelled: its ring is drawn under it (the boss's is BEHAVIOR.BOSS)
    },
    state = {
        IN_TALK = 0x10,              -- in a talk (no speech-bubble cursor while it is set); a creature's cast sets it as its action starts
        PLAYER_HERO = 0x40,          -- a player's hero (its own reactions off); the companions' control switch moves it
        FIGHT_STANCE = 0x8000,       -- the fight stance: enemies are about (what `creature:in_combat()` reads)
        HELD = 0x40000,              -- held by a stun, knock-down or knock-back
        SMITH = 0x1000,              -- a smith's trader (hammer cursor, smith window)
        MERCHANT = 0x2000,           -- a merchant (bag cursor, trade window)
        COMBAT_MASTER = 0x4000,      -- a combat art master (combo cursor)
        HORSE_TRADER = 0x4000000,    -- the horse trader (CreateNPC tag 0x64)
    },
}

-- Enums, by name. The AI states (+0xFC) and the actions (+0x150) are the switches of the creatures' update (Sacred.exe 0x5375C0); the
-- classes are what the exe's class predicates test in the creature record (+4, 0x426650..0x426AD0); the kinds are the item record's
-- +0x2E (what the shop's picker calls categories); the slots are the creature's equipment words (+0x1A4). Names follow the exe's
-- handlers and the two open reimplementations; the comment says where a name rests on a test in the exe only by its number.
world.ENUMS = {
    ai_state = {
        IDLE = 0,            -- 0x542B20
        GO_TO = 1,           -- walks to a target to act on it: a creature, door, container, item or horse (0x53A170)
        BEHAVIOR = 2,        -- a behavior or an ordered walk (0x4EE030)
        MELEE = 4,           -- a melee fight (0x4E0300)
        CAST = 6,            -- a spell's cast (0x51CFD0)
        RANGED = 8,          -- a fight at range (0x4DBFC0)
        DEAD = 9,            -- 0x53E7C0
        TALK = 10,           -- 0x529C60
        WEAPON_ART = 11,     -- a weapon art ordered (0x4FD0F0)
        PICK_UP = 12,        -- picks up items (0x4EC560)
    },
    action = {
        STAND = 0,           -- 0x544890
        MOVE = 1,            -- 0x4F4770
        MELEE_STRIKE = 3,    -- 0x4E4CF0
        CAST = 4,            -- 0x51D150
        SHOOT = 5,           -- 0x4DCB80
        DYING = 6,           -- the death's action (0x53ECA0)
        TALK = 7,            -- 0x52AB70
        GLADIATOR = 8,       -- the hero classes' own arts (handlers 0x4FF0B0 ... 0x5192E0)
        SERAPHIM = 9,
        DARK_ELF = 10,
        WOOD_ELF = 11,
        BATTLE_MAGE = 12,
        VAMPIRESS = 13,
        DWARF = 14,
        DAEMON = 15,
    },
    creature_class = {       -- Creature.pak's class byte; 1, 2, 3, 4, 5 and 6 are tested by the exe (0x426650 ... 0x426AD0), the rest are named by the reimplementation
        HERO = 1, MONSTER = 2, NPC = 3, HORSE = 4, UNDEAD = 5, ANIMAL = 6, MERCENARY = 7, GOBLIN = 8,
        DEMON = 9, DRAGON = 10, ENERGY = 11, ELF = 12, ALL_ENEMIES = 13, HUMAN = 14, DRYAD = 15,
    },
    creature_type_flags = {  -- Creature.pak's flag word (+6)
        FLIES = 0x1,         -- 0x426CF0: the crows, the giant fly, the hornets
        GHOST = 0x20,        -- tested by the death's action and the melee fight (0x43AD80's record)
    },
    item_kind = {            -- the type record's kind (+0x2E); WEAPON ... GLOVES also read by `gear.world.item().category`
        WORLD_OBJECT = 1, CREATURE = 3, CONTAINER = 4, WEAPON = 5, CHEST_ARMOR = 6, RING = 8, POTION = 9, DOOR = 10,
        EFFECT = 12, SHIELD = 13, KEY = 14, QUEST_ITEM = 15, BOOK = 16, HELMET = 17, FOOT_ARMOR = 18, BELT = 19,
        AMULET = 20, SHOULDER_ARMOR = 21, ARM_ARMOR = 22, LEG_ARMOR = 23, GLOVES = 24, WINGS = 25, SMITHING_ACTION = 26,
        PROJECTILE = 27, RUNE = 28, HORSE_EQUIPMENT = 29, DWARF_CANNON = 33,
    },
    equipment_slot = {       -- the creature's 19 equipment words, as `creature:carry(slot, item)` takes them
        HEAD = 0, BODY = 1, BELT = 2, FEET = 3, GLOVES = 4, ARMS = 5, AMULET_1 = 6, AMULET_2 = 7,
        RING_1 = 8, RING_2 = 9, RING_3 = 10, RING_4 = 11, WEAPON = 12, SHIELD = 13, CANNON = 14,
        SHOULDER = 15, LEGS = 16, WINGS = 17, MOUNT = 18,
    },
}

-- True where `word` has any bit of `mask`: `gear.world.has_flag(c:info().flags, gear.world.FLAGS.object.DIALOGUE_ICON)`.
function world.has_flag(word, mask)
    return bit.band(word or 0, mask) ~= 0
end

-- Custom classes: which hero classes' action handlers know a combat art (read from the handlers' switch cases in the exe:
-- Seraphim 0x5038F0, Gladiator 0x4FF0B0, Battle Mage 0x50CD10, Dark Elf 0x506770, Wood Elf 0x50AC00, Vampiress 0x50E6C0, Dwarf
-- 0x514790, Daemon 0x5192E0), the arts tied to class state (forms, firearms) and the arts' tiers for a body. Spells (ids below
-- 1000) go through the spell engine and belong to nobody.
kernel.classes = {}
-- A text given as a string or {en = ..., de = ...}: the string for the game's language (else en, else any).
function kernel.pick_text(text)
    if type(text) == 'table' then
        local lang = tostring(native.game_config().LANGUAGE or 'EN'):lower()
        local pick = text[lang] or text.en
        if pick == nil then pick = select(2, next(text)) end
        text = pick
    end
    assert(type(text) == 'string', 'text must be a string or a table of languages')
    return text
end
do
    local C = kernel.classes
    local RANGES = {
        {1, {1003, 1013, 1017, 1018, 1019, 1020, 1021, 1054, 1069, 1070, 1071, 1072}},
        {2, {1003, 1010, 1012, 1013, 1014, 1015, 1016, 1054, 1057, 1058, 1059, 1060}},
        {3, {1000, 1001, 1002}},
        {4, {1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1065, 1066, 1067, 1068}},
        {5, {1003, 1004, 1005, 1006, 1007, 1008, 1061, 1062, 1063}},
        {6, {1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041,
             1073, 1074, 1075, 1076}},
        {8, {1078, 1079, 1080, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089}},
        {9, {1091, 1092, 1094, 1095, 1096, 1097, 1098, 1099, 1100, 1101, 1102, 1103}},
    }
    local HOME = {}
    for _, r in ipairs(RANGES) do
        for _, id in ipairs(r[2]) do
            local list = HOME[id]
            if not list then list = {}; HOME[id] = list end
            list[#list + 1] = r[1]
        end
    end
    -- class state: Vampiress forms and night arts, Daemon forms, Dwarf firearms and stances
    local STATE = {}
    for _, id in ipairs({1022, 1023, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1073, 1074, 1075, 1076, 1085, 1086, 1087, 1097,
                         1098, 1099, 1100, 1101, 1102, 1103}) do STATE[id] = true end

    -- The hero types (1..9) whose handlers know the art.
    function C.homes(art)
        local out = {}
        for i, t in ipairs(HOME[art] or {}) do out[i] = t end
        return out
    end

    -- The tier of an ability for a body (a hero type 1..9 or its name): 'home' (the body's own art), 'A' (a spell, or an art
    -- that works as is), 'B' (needs ability.route and motion remap), 'C' (class state: stays with its class), 'D' (no hero
    -- handler knows it). Spells and other classes' arts: see docs/CustomClasses.md.
    function C.tier(art, body)
        local t = body
        if type(body) == 'string' then
            for _, c in ipairs(CLASSES) do if c.name == body then t = c.id end end
        end
        if art < 1000 then return 'A' end
        local homes = HOME[art]
        if not homes then return 'D' end
        for _, h in ipairs(homes) do
            if h == t or (t == 7 and h == 6) then return 'home' end
        end
        if STATE[art] then return 'C' end
        return 'B'
    end

    local function body_id(body)
        if type(body) == 'number' then return body end
        for _, c in ipairs(CLASSES) do if c.name == body then return c.id end end
        error('unknown body ' .. tostring(body), 3)
    end
    C.body_id = body_id

    -- The raw Creature.pak hero record (0x56 bytes) of a body, as a string.
    function C.hero_record(body) return native.hero_record(body_id(body)) end

    -- The skills of a body's Creature.pak record: {start = {2 skill ids}, available = {up to 16 skill ids}} (u8 ids; offsets +0x14
    -- and +0x16 of the 0x56-byte record; the available list includes the two start skills).
    function C.hero_skills(body)
        local r = native.hero_record(body_id(body))
        if not r or #r < 0x26 then return nil end
        local available = {}
        for i = 0x17, 0x26 do
            local b = r:byte(i)
            if b ~= 0 then available[#available + 1] = b end
        end
        return {start = {r:byte(0x15), r:byte(0x16)}, available = available}
    end

    local skills_saved = {}   -- body type -> the original 18 bytes (+0x14 .. +0x25)
    -- Replaces a body's skill lists in the game's hero record (every hero of that body, until restore_hero_skills or the game ends).
    -- `start` are two skill ids, `available` up to 16 (0 pads). Returns true, or nil and a reason.
    function C.set_hero_skills(body, start, available)
        local t = body_id(body)
        if type(start) ~= 'table' or #start ~= 2 or type(available) ~= 'table' or #available > 16 then
            return nil, 'expected two start skills and at most 16 available skills'
        end
        local r = native.hero_record(t)
        if not r or #r < 0x26 then return nil, 'no hero record for that body' end
        local bytes = {start[1], start[2]}
        for i = 1, 16 do bytes[#bytes + 1] = available[i] or 0 end
        for _, v in ipairs(bytes) do
            if type(v) ~= 'number' or v < 0 or v > 255 or v ~= math.floor(v) then return nil, 'skill ids are whole numbers 0..255' end
        end
        if not skills_saved[t] then skills_saved[t] = r:sub(0x15, 0x26) end
        return native.hero_record_set(t, 0x14, string.char(unpack(bytes)))
    end

    function C.restore_hero_skills()
        for t, bytes in pairs(skills_saved) do native.hero_record_set(t, 0x14, bytes) end
        skills_saved = {}
    end

    -- The icon texture name of an ability (the string at record +0x0C: GUI_MOVE_RUNDUM.TGA), nil if it has none.
    function C.icon_name(id)
        local rec = data.abilities[id]
        if not rec then return nil end
        local kind = rec.kind
        local chars = {}
        if kind == 'combat_art' then
            for off = 0x0C, 0x2C, 4 do
                local v = rec[('unk_%02x'):format(off)] or 0
                for i = 0, 3 do chars[#chars + 1] = v % 256; v = math.floor(v / 256) end
            end
        else
            for off = 0x0C, 0x2E, 2 do
                local v = rec[('unk_%02x'):format(off)] or 0
                chars[#chars + 1] = v % 256
                chars[#chars + 1] = math.floor(v / 256)
            end
        end
        local out = {}
        for _, b in ipairs(chars) do
            if b == 0 then break end
            out[#out + 1] = string.char(b)
        end
        local name = table.concat(out)
        return name ~= '' and name or nil
    end

    -- The three sprites of an ability's icon (UI images of 64 x 64 in the GUI_spell sheets): the game's action bar and combat art windows draw
    -- the first as the unlearned / disabled one (a grey disc), the second while the art regenerates (the disc pale), the third when it is
    -- available (coloured). T1 (combat arts) keeps them as icon_frame_a / _b / _main, T2 (spells) as icon_frame_normal / _hover / _pressed.
    local ICON_STATES = {disabled = 1, cooldown = 2, available = 3}
    local function icon_frames(rec)
        local a, b, c
        if rec.kind == 'combat_art' then a, b, c = rec.icon_frame_a, rec.icon_frame_b, rec.icon_frame_main
        else a, b, c = rec.icon_frame_normal, rec.icon_frame_hover, rec.icon_frame_pressed end
        if a and b and c and a > 0 and b == a + 1 and c == a + 2 then return {a, b, c} end
        return nil
    end

    -- The image id (for gear.ui.image) of an ability's icon in `state` ('available' (default), 'cooldown' or 'disabled'): the game's own sprite
    -- for that state, drawn as it is (the tint is white). An ability whose icon was changed (a copy with another icon string) has a plain
    -- 64 x 64 texture: that one, with a tint for the state (the game's own icons are grey discs). nil if it has none.
    function C.art_icon(id, state)
        local idx = ICON_STATES[state or 'available']
        if not idx then error("gear.classes.art_icon: state must be 'available', 'cooldown' or 'disabled'", 2) end
        local rec = data.abilities[id]
        if not rec then return nil end
        local base = rec.base and data.abilities[rec.base] or nil
        local frames = icon_frames(rec)
        if frames and (not base or C.icon_name(id) == C.icon_name(rec.base)) then
            local image = frames[idx]
            if native.ui_image_info(image) then return image, 0xFFFFFFFF end
        end
        local name = C.icon_name(id)
        local image = name and native.texture_image(name) or nil
        if not image then return nil end
        local gold = id >= 1000
        local tint = idx == 3 and (gold and 0xFFFFD060 or 0xFFB8D0FF) or idx == 2 and (gold and 0xFFA08840 or 0xFF8090A8) or 0xFF707070
        return image, tint
    end

    -- A hero skill by id (the u8 ids of Creature.pak's skill lists): {id, name, description} from the game's texts (the skills window's
    -- own: name text 0x24B7 + id, description 0x24E9 + id); nil if the game has no skill with that id. The game has no icons for skills.
    function C.skill(id)
        if type(id) ~= 'number' or id < 1 or id > 255 then return nil end
        local name = native.text_get(0x24B7 + id)
        if not name or name == '' or name:find('EMPTY', 1, true) then return nil end
        local desc = native.text_get(0x24E9 + id)
        if desc and desc:find('EMPTY', 1, true) then desc = nil end
        return {id = id, name = name, description = desc}
    end

    -- Every skill the game has, by id.
    function C.skills()
        local out = {}
        for id = 1, 64 do
            local s = C.skill(id)
            if s then out[#out + 1] = s end
        end
        return out
    end

    -- Runes for art copies. The game teaches an art from an item type whose record names it (+0x6A). A copy (alias 2000..3799) gets an item type of
    -- Gear's own, 6200 + (alias - 2000): a copy of the type record of the base art's rune, teaching the alias, named like the copy.
    C.RUNE_TYPE_FIRST, C.RUNE_TYPE_LAST = 6200, 7999
    C.rune_types = {}     -- alias -> item type
    function C.rune_type_of(alias)
        return C.rune_types[alias]
    end
    function C.define_rune(alias, opts)
        opts = opts or {}
        local rec = data.abilities[alias]
        if not rec or not kernel.alias_owner[alias] then return nil, 'not an ability copy of this mod' end
        local type_id = C.RUNE_TYPE_FIRST + (alias - 2000)
        if alias < 2000 or type_id > C.RUNE_TYPE_LAST then return nil, 'rune types exist for copies 2000..3799' end
        local base_runes = C.runes()[rec.base]
        local base_type = opts.base_type or (base_runes and base_runes[1])
        if not base_type then
            -- no rune of its own: any rune of the same kind will do
            for _, list in pairs(C.runes()) do base_type = list[1] break end
        end
        local ok, err = native.item_define_type(type_id, base_type, alias, opts.name_text or rec.name_text)
        if not ok then return nil, err end
        C.rune_types[alias] = type_id
        local runes = C.runes()
        runes[alias] = {type_id}
        return type_id
    end
    -- The art an item type teaches (type record +0x6A), 0 / nil for items that are no runes.
    function C.rune_art_of_type(item_type)
        for art, list in pairs(C.runes()) do
            for _, t in ipairs(list) do if t == item_type then return art end end
        end
        return nil
    end

    local rune_cache
    -- {[ability id] = {item type, ...}}: the rune items that teach an ability (item types whose record names one: type record +0x6A).
    function C.runes()
        if rune_cache then return rune_cache end
        local out = {}
        for t = 1, 0x7E5F do
            local id = native.table_read('object_types', t, 0x6A, 'u16')
            if id and id ~= 0 and (native.table_read('object_types', t, 0x2E, 'u8') == 0x1C or id >= 1 and id < 1200) then
                local list = out[id]
                if not list then list = {}; out[id] = list end
                list[#list + 1] = t
            end
        end
        rune_cache = out
        return out
    end

    -- Every ability: {id, kind = 'combat_art'|'spell', name (the game's text), name_text, description_text, target_mode, homes = {types},
    -- tiers = {[hero type] = tier}, runes = {item types}, alias = base id | nil}.
    function C.catalog()
        local list, runes = {}, C.runes()
        local A = data.abilities
        for _, id in ipairs(A:ids()) do
            local a = A[id]
            local tiers = {}
            for _, c in ipairs(CLASSES) do tiers[c.id] = C.tier(id, c.id) end
            list[#list + 1] = {
                id = id, kind = a.kind, name = native.text_get(a.name_text), name_text = a.name_text, description_text = a.description_text,
                target_mode = a.target_mode, homes = C.homes(id), tiers = tiers, runes = runes[id] or {},
                alias = kernel.alias_owner[id] and a.base or nil,
            }
        end
        return list
    end

    function kernel.release_aliases(mod)
        for id, owner in pairs(kernel.alias_owner) do
            if owner == mod then
                kernel.alias_owner[id] = nil
                if kernel.art_spell_alias then kernel.art_spell_alias[id] = nil end
                native.alias_remove(id)
            end
        end
    end
end

-- The hero's class: name ('darkelf' ...) and id, nil for other creatures.
function creature_mt:class()
    local id = native.hero_class(self.id)
    local c = id and CLASS_INFO.by_id[id]
    if c then return c.name, c.id end
end

-- Party (gear.party): companion heroes in single player, see docs/GearApi.md, "Party". Kept in kernel.party: the main
-- chunk is at Lua's 200-locals limit.
-- Buffs (gear.buffs): changes to a creature's combat values for a while, with the game's own look on the hero. Kept
-- in kernel.buffs: the main chunk is at Lua's 200-locals limit. Definitions are shared by every mod (an id is
-- 'mod_id:name' unless it names another mod's); apply them to any creature at a strength.
kernel.buffs = {}
do
    local B = kernel.buffs
    -- The stats block's float fields cStats::recalc works out (see src/gear/world/buffs.h), by name: sums (0 before) and
    -- factors (1 before). Seen with the game's own buffs: Steinhaut (resist_physical, armor_factor), Flammenhaut
    -- (resist_fire), Schildwall (every resistance), Zwergenpanzer (resist_fire, resist_poison), Hoellenkraft
    -- (damage_min_fire, damage_max_fire, attack_factor), Wieselflink (speed_factor), Aue des Geistes (regen factors).
    B.STATS = {
        resist_physical = 0x66, resist_fire = 0x6A, resist_magic = 0x6E, resist_poison = 0x72,
        damage_min_physical = 0x76, damage_min_fire = 0x7A, damage_min_magic = 0x7E, damage_min_poison = 0x82,
        damage_max_physical = 0x86, damage_max_fire = 0x8A, damage_max_magic = 0x8E, damage_max_poison = 0x92,
    }
    B.FACTORS = {
        attack_factor = 0x9A, speed_factor = 0xE6, armor_factor = 0xEA, regen_factor = 0xEE, art_regen_factor = 0xF2,
    }
    -- The speed values (int16 in the stats block; 100 is normal, the animation runs value / 100 as fast). The engine
    -- caps them in cStats::recalc (220, 260 with creature flag 0x10000, movement 380 for some creatures); a buff's
    -- speeds are added after the cap, so they lift it. Clamped to 0..1000.
    B.SPEEDS = {attack_speed = 0x3C, cast_speed = 0x3E, move_speed = 0x40}
    -- The spell overlays the game draws a hero with (creature +0x60).
    B.OVERLAYS = {light = 6, stone = 7, flame = 10}
    B.defs = {}
    B.order = {}
    B.meta = {}

    local function offset_of(name, factor, speed)
        if type(name) == 'number' then return name end
        if speed then return B.SPEEDS[name] end
        return (factor and B.FACTORS or B.STATS)[name]
    end

    function B.define(m, def)
        assert(type(def) == 'table' and type(def.id) == 'string' and def.id ~= '', 'a buff needs an id')
        local id = def.id:find(':') and def.id or (m.id .. ':' .. def.id)
        for name, v in pairs(def.stats or {}) do
            assert(offset_of(name, false), 'unknown stat ' .. tostring(name))
            assert(type(v) == 'number', 'stat values are numbers')
        end
        for name, v in pairs(def.factors or {}) do
            assert(offset_of(name, true), 'unknown factor ' .. tostring(name))
            assert(type(v) == 'number', 'factors are numbers')
        end
        for name, v in pairs(def.speeds or {}) do
            assert(offset_of(name, false, true), 'unknown speed ' .. tostring(name))
            assert(type(v) == 'number', 'speeds are numbers')
        end
        for name, v in pairs(def.speed_factors or {}) do
            assert(offset_of(name, false, true), 'unknown speed ' .. tostring(name))
            assert(type(v) == 'number', 'speed factors are numbers')
        end
        local overlay = def.overlay
        if type(overlay) == 'string' then overlay = assert(B.OVERLAYS[overlay], 'unknown overlay ' .. overlay) end
        local d = {id = id, name = def.name or def.id, description = def.description, icon = def.icon,
            stats = def.stats or {}, factors = def.factors or {}, speeds = def.speeds or {},
            speed_factors = def.speed_factors or {}, overlay = overlay or 0, seconds = def.seconds or 0,
            debuff = def.debuff and true or false, mod = m.id}
        if not B.defs[id] then B.order[#B.order + 1] = id end
        B.defs[id] = d
        return d
    end

    function B.definitions()
        local out = {}
        for _, id in ipairs(B.order) do out[#out + 1] = B.defs[id] end
        return out
    end

    -- The mods {offset, factor, value} of a definition at `strength` (1: as defined; a factor f becomes
    -- 1 + (f - 1) * strength).
    local function mods_of(d, strength)
        local list = {}
        for name, v in pairs(d.stats) do list[#list + 1] = {offset_of(name, false), false, v * strength} end
        for name, v in pairs(d.factors) do list[#list + 1] = {offset_of(name, true), true, 1 + (v - 1) * strength} end
        for name, v in pairs(d.speeds or {}) do list[#list + 1] = {offset_of(name, false, true), false, v * strength} end
        for name, v in pairs(d.speed_factors or {}) do
            list[#list + 1] = {offset_of(name, false, true), true, 1 + (v - 1) * strength}
        end
        return list
    end

    function B.apply(m, target, buff, opts)
        opts = opts or {}
        local id = type(target) == 'table' and target.id or target
        local d = type(buff) == 'table' and buff or B.defs[buff] or B.defs[m.id .. ':' .. tostring(buff)]
        if not d then return nil, 'no such buff' end
        if not d.mod then d = B.define(m, d) end
        local strength = opts.strength or 1
        local key = opts.key or d.id
        local seconds = opts.seconds or d.seconds or 0
        local source = opts.source and (type(opts.source) == 'table' and opts.source.id or opts.source) or 0
        local ok, err = native.buff_apply(id, key, mods_of(d, strength), seconds, opts.overlay or d.overlay or 0, source,
            opts.ability or 0)
        if not ok then return nil, err end
        B.meta[id .. '|' .. key] = {def = d.id, strength = strength}
        kernel.dispatch('buff.applied', world.object(id), key, d.id, strength)
        return true
    end

    function B.remove(target, key)
        local id = type(target) == 'table' and target.id or target
        return native.buff_remove(id, key)
    end

    function B.list(target)
        local id = type(target) == 'table' and target.id or target
        local list = native.buff_list(id) or {}
        for _, b in ipairs(list) do
            local meta = B.meta[id .. '|' .. b.key]
            b.id = meta and meta.def or b.key
            b.strength = meta and meta.strength or 1
            b.source = b.source ~= 0 and world.object(b.source) or nil
            b.ability = b.ability ~= 0 and b.ability or nil
        end
        return list
    end

    -- The creature's combat value fields now, by offset ({[0x66] = 12.5, ...}).
    function B.fields(target)
        return native.buff_fields(type(target) == 'table' and target.id or target)
    end

    -- The creature's speed values now, by name ({attack_speed = 140, cast_speed = 120, move_speed = 130}), past the engine's
    -- cap when a buff lifted it. nil when it is no creature.
    function B.speeds(target)
        local raw = native.buff_speeds(type(target) == 'table' and target.id or target)
        if not raw then return nil end
        local out = {}
        for name, o in pairs(B.SPEEDS) do out[name] = raw[o] end
        return out
    end

    -- What changed between two `fields` snapshots, as a definition's stats and factors (by offset): copies a buff the
    -- game gave (cast it, compare before and after) so it can go onto someone else.
    function B.change(before, after)
        local stats, factors, is_factor = {}, {}, {}
        for _, o in pairs(B.FACTORS) do
            is_factor[o] = true
            local a, b = before[o], after[o]
            if a and b and a ~= 0 and math.abs(b - a) > 1e-4 then factors[o] = b / a end
        end
        for o, b in pairs(after) do
            local a = before[o]
            if a and not is_factor[o] and math.abs(b - a) > 1e-4 then stats[o] = b - a end
        end
        return stats, factors
    end

    function B.overlay(target)
        return native.buff_overlay(type(target) == 'table' and target.id or target)
    end
    function B.set_overlay(target, which, seconds)
        if type(which) == 'string' then which = B.OVERLAYS[which] end
        return native.buff_set_overlay(type(target) == 'table' and target.id or target, which or 0, seconds or 30)
    end

    function B.poll()
        if not native.buff_ended then return end
        for _, e in ipairs(native.buff_ended()) do
            local key = e[1] .. '|' .. e[2]
            local meta = B.meta[key]
            B.meta[key] = nil
            kernel.dispatch('buff.ended', world.object(e[1]), e[2], meta and meta.def or e[2])
        end
    end

    function creature_mt:buffs() return B.list(self) end
    function creature_mt:remove_buff(key) return B.remove(self, key) end
end

kernel.party = {}
do
    local P = kernel.party
    local ACTIONS = {[0] = 'idle', [1] = 'attacking', [2] = 'walking', [4] = 'attacking', [8] = 'attacking', [11] = 'ability'}
    function creature_mt:is_hero() return native.hero_class(self.id) ~= nil end
    -- The combat arts (ids from 1000) and spells the hero knows: {id, level, bonus, regen, regen_max, art, ready}.
    function creature_mt:abilities()
        local list = native.party_abilities(self.id)
        if not list then return nil end
        for _, a in ipairs(list) do
            a.art = a.id >= 1000
            a.ready = a.regen <= 0
        end
        return list
    end
    function creature_mt:ability(id)
        for _, a in ipairs(self:abilities() or {}) do
            if a.id == id then return a end
        end
    end
    -- The hero's combos (the combo master's slots with arts): {id, slot, arts, levels, total, charge, ready}; id is what
    -- order_ability takes.
    function creature_mt:combos()
        local list = native.party_combos(self.id)
        if not list then return nil end
        for _, k in ipairs(list) do k.ready = k.charge >= k.total end
        return list
    end
    -- What the hero wears: {slot = n (0..18), item = gear.world.item table}, in slot order.
    function creature_mt:equipment()
        local slots = native.party_equipment(self.id)
        if not slots then return nil end
        local out = {}
        for slot = 0, 18 do
            local id = slots[slot]
            local it = id and world.item(id)
            if it then out[#out + 1] = {slot = slot, item = it} end
        end
        return out
    end
    -- What any creature carries: the items in its equipment words (monsters hold the gear a hero can disarm from them there).
    function creature_mt:carried()
        local slots = native.object_carried(self.id)
        if not slots then return nil end
        local out = {}
        for slot = 0, 18 do
            local id = slots[slot]
            local it = id and world.item(id)
            if it then out[#out + 1] = {slot = slot, item = it} end
        end
        return out
    end
    -- The equipment slot (0..18) an item goes to for this hero (the engine's own rule: the first slot of the item's group, i.e.
    -- 6 for amulets (6, 7), 8 for rings (8..11), 12 for weapons, 13 for shields), nil if it cannot wear it.
    function creature_mt:equip_slot(item)
        local item_type = type(item) == 'table' and item.type or item
        if type(item_type) ~= 'number' then error('equip_slot wants an item or an item type', 2) end
        return native.item_equip_slot(self.id, item_type)
    end
    -- creature:learn(id, level) / creature:forget(id): add or remove an art (id from 1000) or spell of a hero's abilities with the
    -- engine's own function; true, or nil and a reason (it knows it already / does not know it / not a hero).
    function creature_mt:learn(ability, level)
        assert(type(ability) == 'number', 'expected an ability id')
        return native.creature_learn(self.id, ability, level or 1)
    end
    -- creature:use_item(item): the creature uses an item of its backpack as a click on it does: a potion is drunk, a rune or a scroll read.
    -- `item` is an item id or a table with `id`. Returns true, or nil and why.
    function creature_mt:use_item(item)
        local id = type(item) == 'table' and item.id or item
        assert(type(id) == 'number', 'expected an item or its id')
        return native.item_use(self.id, id)
    end
    function creature_mt:forget(ability)
        assert(type(ability) == 'number', 'expected an ability id')
        return native.creature_forget(self.id, ability)
    end
    -- creature:raise(id, levels): raises the level of an art or spell the hero knows by `levels` (default 1) with the engine's own function
    -- (the one the skill purchase uses); true, or nil and a reason (it does not know it / the level would pass 255 / not a hero).
    function creature_mt:raise(ability, levels)
        assert(type(ability) == 'number', 'expected an ability id')
        return native.creature_raise(self.id, ability, levels or 1)
    end
    -- creature:set_combo_charge(slot, charge): how far the combo of `slot` (0..3) has regenerated, in seconds of its `total` (ready when
    -- charge >= total; not clamped above). Party heroes of the local player only; true, or nil and a reason.
    function creature_mt:set_combo_charge(slot, charge)
        assert(type(slot) == 'number' and type(charge) == 'number', 'expected a slot (0..3) and a charge in seconds')
        return native.party_set_combo_charge(self.id, slot, charge)
    end
    function creature_mt:set_combo(slot, arts)
        assert(type(slot) == 'number' and type(arts) == 'table', 'expected a slot (0..3) and a list of combat arts')
        return native.party_set_combo(self.id, slot, arts)
    end
    -- What the hero does: 'idle', 'walking', 'attacking', 'ability' or 'busy', and the raw action and movement states.
    function creature_mt:action()
        local action, movement = native.party_state(self.id)
        if not action then return nil end
        local name = ACTIONS[action] or 'busy'
        if name == 'idle' and movement ~= 0 then name = 'walking' end
        return name, action, movement
    end
    function creature_mt:order_move(x, y)
        assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
        local o = info(self)
        return native.party_order(self.id, 'move', 0, math.floor(x), math.floor(y), o and o.layer or 0, 0)
    end
    -- creature:gait([value]): how the creature moves, 1 walk, 2 run (read; with a value, set first). The engine sets it by itself when an order
    -- starts (a creature of the game's own AI runs while it chases, and walks otherwise), so set it after the order.
    function creature_mt:gait(value)
        return native.gait(self.id, value)
    end
    function creature_mt:pickup(item) return native.party_pickup(self.id, type(item) == 'table' and item.id or item) end
    function creature_mt:order_stop() return native.party_order(self.id, 'stop') end
    function creature_mt:order_attack(target)
        local id = type(target) == 'table' and target.id or target
        assert(type(id) == 'number', 'target must be a creature or its id')
        return native.party_order(self.id, 'attack', id)
    end
    -- order_ability(id, target) or order_ability(id, x, y): a combat art or spell on a creature or a spot.
    function creature_mt:order_ability(ability, target, y)
        assert(type(ability) == 'number', 'ability must be an id')
        if type(target) == 'table' then target = target.id end
        if y ~= nil then
            assert(type(target) == 'number' and type(y) == 'number', 'x and y must be numbers')
            local o = info(self)
            return native.party_order(self.id, 'ability', 0, math.floor(target), math.floor(y), o and o.layer or 0, ability)
        end
        assert(type(target) == 'number', 'target must be a creature, its id, or x, y')
        return native.party_order(self.id, 'ability', target, nil, nil, nil, ability)
    end
    -- set_name(name): the name the game shows for a party hero (1 to 30 characters); not saved for companions.
    function creature_mt:set_name(name)
        assert(type(name) == 'string', 'name must be a string')
        return native.party_set_name(self.id, name)
    end
    -- drink_potion(kind): a party hero drinks a potion of its own backpack ('health' default, or the game's kind number)
    -- The hero's weapon slots (keys 1-5): {active = n, slots = {{main = item?, off = item?}...}} (1-based, items as
    -- gear.world.item tables), nil for other creatures.
    function creature_mt:weapon_slots()
        local active, list = native.party_weapon_slots(self.id)
        if not active then return nil end
        local slots = {}
        for i, s in ipairs(list) do
            slots[i] = {main = s[1] ~= 0 and world.item(s[1]) or nil, off = s[2] ~= 0 and world.item(s[2]) or nil}
        end
        return {active = active + 1, slots = slots}
    end
    -- Takes up the weapons of slot n (1-based), as the keys 1-5 do for the hero you play.
    function creature_mt:use_weapon_slot(n)
        return native.party_use_weapon_slot(self.id, n - 1)
    end
    -- A hero's experience and where its level starts and the next begins (the game's own table).
    function creature_mt:experience_range()
        return native.party_exp_range(self.id)
    end
    function creature_mt:drink_potion(kind)
        local k = kind == nil and 1 or kind == 'health' and 1 or kind
        assert(type(k) == 'number', "kind must be 'health' or a number")
        return native.party_potion(self.id, k)
    end
    -- revive(hp): a fallen party hero stands up again with hp hit points (default: a quarter of its maximum).
    function creature_mt:revive(hp)
        local o = info(self)
        if not o then return nil, 'no such hero' end
        return native.party_revive(self.id, math.floor(hp or math.max(1, (o.max_hp or 4) / 4)))
    end
    function creature_mt:add_experience(amount)
        assert(type(amount) == 'number', 'amount must be a number')
        return native.party_add_exp(self.id, math.floor(amount))
    end
    function creature_mt:is_enemy(other)
        local id = type(other) == 'table' and other.id or other
        assert(type(id) == 'number', 'other must be a creature or its id')
        return native.party_hostile(self.id, id)
    end
    -- The image (gear.ui.image) of the creature's portrait, as the party window shows it.
    function creature_mt:portrait()
        local o = info(self)
        return o and native.party_portrait(o.type)
    end

    -- class_portrait(class) -> image | nil, message: the portrait of a hero class (a name or class_id) for gear.ui.image.
    function P.class_portrait(class)
        local id = class
        if type(class) == 'string' then
            id = nil
            for _, c in ipairs(CLASSES) do
                if c.name == class then id = c.id end
            end
        end
        if type(id) ~= 'number' or not CLASS_INFO.by_id[id] then return nil, 'no such hero class' end
        return native.party_portrait(id)
    end

    P.FIRST_ID, P.LAST_ID = 3, 16
    function P.available() return native.party_available() end
    function P.heroes()
        local out = {}
        for i, id in ipairs(native.party_heroes()) do out[i] = wrap(id) end
        return out
    end
    function P.leader() return wrap(native.hero()) end
    -- The game's own mercenaries that follow the party (the hero they follow can be any party hero).
    function P.mercenaries()
        local out = {}
        for i, id in ipairs(native.party_mercenaries()) do out[i] = wrap(id) end
        return out
    end
    function P.companions()
        local out = {}
        for i, id in ipairs(native.party_heroes()) do
            if i > 1 then out[#out + 1] = wrap(id) end
        end
        return out
    end
    -- recruit(class, opts) -> creature | nil, message. class: a name ('darkelf') or id; opts: {x, y, layer} (default:
    -- where the leader stands), experience (given through the game's own gain, so it levels up with skill points).
    function P.recruit(class, opts)
        opts = opts or {}
        local id = class
        if type(class) == 'string' then
            id = nil
            for _, c in ipairs(CLASSES) do
                if c.name == class then id = c.id end
            end
        end
        if type(id) ~= 'number' or not CLASS_INFO.by_id[id] then return nil, 'no such hero class' end
        local leader = native.hero()
        local lo = leader and native.object(leader)
        local x, y, layer = opts.x or (lo and lo.x), opts.y or (lo and lo.y), opts.layer or (lo and lo.layer) or 0
        if not x or not y then return nil, 'no game is running' end
        local made, err = native.party_recruit(id, math.floor(x), math.floor(y), layer)
        if not made then return nil, err end
        if opts.experience and opts.experience >= 1 then
            native.party_add_exp(made, math.floor(opts.experience))
        end
        return wrap(made)
    end
    function P.dismiss(c)
        local id = type(c) == 'table' and c.id or c
        assert(type(id) == 'number', 'expected a creature or its id')
        return native.party_dismiss(id)
    end
    -- control(c) -> true | nil, message; every mod gets 'party.control' (from, to) after a switch.
    function P.control(c)
        local id = type(c) == 'table' and c.id or c
        assert(type(id) == 'number', 'expected a creature or its id')
        local from = native.hero()
        local ok, err = native.party_control(id)
        if ok and from ~= id then kernel.dispatch('party.control', wrap(from), wrap(id)) end
        return ok, err
    end
    -- shared_purse(on?) -> boolean: whether `control` moves the gold to the hero you play (default true); a boolean sets it.
    function P.shared_purse(on)
        assert(on == nil or type(on) == 'boolean', 'on must be a boolean')
        return native.party_shared_purse(on)
    end
    -- give_gold(from, to, amount) -> true | nil, message: moves gold between two party heroes (single player).
    function P.give_gold(from, to, amount)
        local a = type(from) == 'table' and from.id or from
        local b = type(to) == 'table' and to.id or to
        assert(type(a) == 'number' and type(b) == 'number', 'expected creatures or their ids')
        assert(type(amount) == 'number', 'amount must be a number')
        return native.party_give_gold(a, b, math.max(0, math.floor(amount)))
    end
    -- share_experience(share, radius): experience a party hero gains from the game also goes to the others within
    -- `radius` world units (0 or nil: anywhere), times `share` (0 turns it off, 1 the full amount).
    function P.share_experience(share, radius)
        assert(type(share) == 'number', 'share must be a number')
        native.party_share(share, radius or 0)
    end
    -- experience_scale(factor): every experience gain the game makes for a hero (kills, quests) is multiplied by `factor`.
    function P.experience_scale(factor)
        assert(type(factor) == 'number' and factor >= 0, 'factor must be a number, 0 or more')
        native.party_experience_scale(factor)
    end
end

