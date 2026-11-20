-- Sacred Gear's kernel: the trusted Lua side of the host (lua_host.cpp runs it with the full standard library).
-- It finds and orders the mods, gives each one its own sandboxed environment, keeps the event handlers and calls
-- them. Mods never see this chunk's globals, the `native` table or each other's environments.
--
-- `native` (set by lua_host.cpp before this chunk runs):
--   log(text)                         a line in SacredGear.log
--   mod_dirs()                        the folder names under mods\ that hold a mod.lua, in no particular order
--   read_file(dir, relative)          a file of mod folder `dir` as a string, or nil and a message
--   load(text, chunkname, env)        a text chunk (never bytecode) with `env` as its environment, or nil and a message
--   time()                            seconds since an arbitrary start, high resolution
--   reset_budget()                    gives the next handler the whole time budget of the current kernel call
--   table_info(name)                  record count and size of an engine table, or nil if it is unknown
--   table_read(name, index, offset, kind)  one field of a record (kind: u8 i8 u16 i16 u32 i32 f32), or nil
--   table_string(name, index, offset, max) a zero-terminated string of a record, or nil
--   hero()                            the local hero's object id, or nil
--   world_changes()                   whether the world may be changed now (a single player game runs)
--   object(id)                        {id, type, flags, x, y, layer, creature, hp, max_hp, gold, exp, level} or nil
--   creatures_near(x, y, layer, radius, limit)  ids of the creatures there, nearest first
--   spawn(type, x, y, layer)          the new object's id, or nil and a message
--   teleport(id, x, y, layer)         true, or nil and a message
--   item_type_text(type)              the tooltip text of a scratch item of an item type, or nil and a message
--   remove(id)                        true, or nil and a message (a creature other than a hero is deleted)
--   set_health(id, hp), set_gold(id, gold)  true, or nil and a message
--   pipe_open(name), pipe_close(h), pipe_send(h, text), pipe_poll(h, limit), pipe_connected(h)
--   version                           Sacred Gear's version string

local native = ...
local kernel = {slot_reservations = {}}   -- slot_reservations: mod -> its gear.game.reserve_slots list

local MAX_ERRORS = 10   -- a mod whose handlers raised this many errors is switched off for the session

local mods = {}         -- loaded mods in load order: {id, name, version, dir, env, errors, disabled}
kernel.savegame = {scope = nil, kind = nil}      -- the savegame this game was loaded from or last saved to; 'load' | 'hero' | 'new'
local handlers = {}     -- event name -> array of {mod, fn}
local current           -- the mod whose code runs at the moment (for messages)
local restore_abilities    -- defined with the ability tables below

local function log(fmt, ...)
    native.log(string.format(fmt, ...))
end

-- String metatable: mods share it with the kernel, so they must not reach it (getmetatable("").__index is the
-- real string library).
getmetatable("").__metatable = false
math.randomseed(os.time())

local function copy(t)
    local c = {}
    for k, v in pairs(t) do c[k] = v end
    return c
end

local function text_of(...)
    local n = select('#', ...)
    local parts = {}
    for i = 1, n do
        parts[i] = tostring((select(i, ...)))
    end
    return table.concat(parts, '\t')
end

local function traceback(err)
    return debug.traceback(tostring(err), 2)
end

-- Calls fn(...) for `mod`, logs an error with its traceback and counts it. Returns ok and fn's first result.
local function call(mod, what, fn, ...)
    local previous = current
    current = mod
    native.reset_budget()
    local ok, result = xpcall(fn, traceback, ...)
    current = previous
    if not ok then
        mod.errors = mod.errors + 1
        log('[%s] error in %s: %s', mod.id, what, result)
        if mod.errors >= MAX_ERRORS and not mod.disabled then
            mod.disabled = true
            if restore_abilities then restore_abilities(mod) end
            if kernel.release_aliases then kernel.release_aliases(mod) end
            log('[%s] %d errors: the mod is switched off until the game restarts', mod.id, mod.errors)
        end
    end
    return ok, result
end

-- Engine tables as read-only record views (M1). Record indices are the engine's, from 0.
local function make_table(name)
    local record_mt = {}
    record_mt.__index = record_mt
    for _, kind in ipairs({'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'f32'}) do
        record_mt[kind] = function(self, offset)
            assert(type(offset) == 'number', 'offset must be a number')
            return native.table_read(name, self.index, offset, kind)
        end
    end
    function record_mt:str(offset, max_length)
        assert(type(offset) == 'number', 'offset must be a number')
        return native.table_string(name, self.index, offset, max_length or 64)
    end
    local view = {}
    function view:count()
        local count = native.table_info(name)
        return count or 0
    end
    function view:stride()
        local _, size = native.table_info(name)
        return size or 0
    end
    function view:get(index)
        assert(type(index) == 'number', 'index must be a number')
        local count = native.table_info(name)
        if not count or index < 0 or index >= count or index % 1 ~= 0 then
            return nil
        end
        return setmetatable({index = index}, record_mt)
    end
    return view
end

local data = {
    creature_types = make_table('creature_types'),
    object_types = make_table('object_types'),
}

-- Object types by name. The type records (0x80 bytes, id = index) hold the internal name at +0x37 and the kind at
-- +0x2E. The names are read once, the first time a mod asks.
local OBJECT_TYPE_NAME, OBJECT_TYPE_KIND = 0x37, 0x2E
local type_index        -- lower-case name -> id (the first id with that name)

function data.object_types:name(id)
    local r = self:get(id)
    local n = r and r:str(OBJECT_TYPE_NAME, 0x49)
    if n == '' then return nil end
    return n
end

-- What to show the player for type `id`: the localized name, else the internal one, else 'type <id>'. Never nil.
function data.object_types:label(id)
    assert(type(id) == 'number', 'id must be a number')
    return self:display_name(id) or self:name(id) or ('type ' .. tostring(id))
end

-- The type id a saved or typed spec stands for: a number (checked to be a type), an internal name (any case), or the localized name
-- of a creature type (any case); nil if it names nothing. For migrating names that older versions of a mod saved to ids.
do
local by_localized
function data.object_types:resolve(spec)
    if type(spec) == 'number' then return self:get(spec) and spec or nil end
    if type(spec) ~= 'string' or spec == '' then return nil end
    local id = self:find(spec)
    if id then return id end
    if not by_localized then
        by_localized = {}
        for t = 1, self:count() - 1 do
            if self:kind(t) == 3 then
                local shown = self:display_name(t)
                if shown and not by_localized[shown:lower()] then by_localized[shown:lower()] = t end
            end
        end
    end
    return by_localized[spec:lower()]
end
end

function data.object_types:kind(id)
    local r = self:get(id)
    return r and r:u8(OBJECT_TYPE_KIND)
end

-- The localized name of type `id`. Items: the first line of the tooltip of a scratch item of that type. Creatures (kind 3):
-- the game's text for the id in the type record's dword +0x20 (the "resource id"; for every creature type the shipped data holds
-- it is the type id itself, and the text table takes it as it is). Colour codes are removed.
do
local display_names = {}
local OBJECT_TYPE_RESOURCE, CREATURE_KIND = 0x20, 3
local function clean(text)
    local BS = string.char(92)       -- the game's codes: BS g + 16 characters (image), BS c / BS w + 8 hex digits (colour), BS + one letter
    local line = text:match('^[^' .. string.char(10, 13) .. ']*') or ''
    line = line:gsub(BS .. 'g' .. string.rep('.', 16), ''):gsub(BS .. '[cw]' .. string.rep('%x', 8), ''):gsub(BS .. '%w', '')
    return (line:gsub('%s*%(%d+/%d+%)%s*$', ''):gsub('^%s+', ''):gsub('%s+$', ''))
end
function data.object_types:display_name(id)
    assert(type(id) == 'number', 'id must be a number')
    local cached = display_names[id]
    if cached ~= nil then return cached or nil end
    local text, err
    if self:kind(id) == CREATURE_KIND then
        local rid = self:get(id):u32(OBJECT_TYPE_RESOURCE)      -- bit 31 set: a hash key, not a text id
        text = rid ~= 0 and rid < 0x80000000 and native.text_get(rid) or nil
        err = 'the creature has no name text'
    else
        text, err = native.item_type_text(id)
    end
    if not text then return nil, err end
    local line = clean(text)
    if line == '' then display_names[id] = false return nil, 'the type has no name' end
    display_names[id] = line
    return line
end
end

local function build_type_index()
    if type_index then return type_index end
    local count = data.object_types:count()
    if count == 0 then return nil end
    type_index = {}
    for id = 1, count - 1 do
        local n = native.table_string('object_types', id, OBJECT_TYPE_NAME, 0x49)
        if n and n ~= '' then
            local key = n:lower()
            if not type_index[key] then type_index[key] = id end
        end
    end
    return type_index
end

-- The id of the type named `name` (any case), or nil.
function data.object_types:find(name)
    assert(type(name) == 'string', 'name must be a string')
    local index = build_type_index()
    return index and index[name:lower()]
end

-- {{id, name, kind}, ...} of the types whose name contains `text` (plain text, any case), at most `limit` (100).
function data.object_types:search(text, limit)
    assert(type(text) == 'string', 'text must be a string')
    limit = limit or 100
    local index = build_type_index()
    local out = {}
    if not index then return out end
    local needle = text:lower()
    for key, id in pairs(index) do
        if key:find(needle, 1, true) then
            out[#out + 1] = {id = id, name = self:name(id), kind = self:kind(id)}
        end
    end
    table.sort(out, function(a, b) return a.id < b.id end)
    while #out > limit do out[#out] = nil end
    return out
end

---------------------------------------------------------------------------------------------------------------
-- The ability tables (gear.data.abilities): the exe's two static record tables, T1 (ids from 1000, 0x78-byte records)
-- and T2 (ids from 1, 0x94-byte records), as one table by id. Fields have names where a consumer proves them
-- (behaviours.md of sacred-gear-reborn-decomp) and `unk_<offset>` otherwise. Writes go to the live tables, are checked
-- for type and range, and are remembered per mod so they can be undone (`restore`, and when the mod is switched off).
---------------------------------------------------------------------------------------------------------------
local KIND_SIZE = {u8 = 1, i8 = 1, u16 = 2, i16 = 2, u32 = 4, i32 = 4, f32 = 4}
-- Field names follow the reverse engineering of the ability records (docs/CustomClasses.md, "Ability record fields"): every
-- byte of a record belongs to a named field; fields that have two names (the older name and the descriptive one) are the same
-- bytes. The unk_<offset> names of the first version still work (they read and write the same bytes).
local ABILITY_TABLES = {
    T1 = {
        native = 'ability_t1', stride = 0x78, unknown = {kind = 'u32', size = 4},
        old_named = {id = 0x00, name_text = 0x04, description_text = 0x08, target_mode = 0x4C, effect_base = 0x50, effect_per_level = 0x54,
            effect2_base = 0x58, effect2_per_level = 0x5C, regen_base = 0x40, regen_per_level = 0x44},
        fields = {
            id = {0x00, 'i16'}, name_text = {0x04, 'u32'},
            description_text = {0x08, 'u32'}, icon_frame_a = {0x30, 'i32'},
            icon_frame_b = {0x34, 'i32'},
            icon_frame_main = {0x38, 'i32'}, design_value_j = {0x3C, 'u32'},
            regen_base = {0x40, 'f32'}, regen_per_level = {0x44, 'f32'}, approach_distance = {0x48, 'i32'},
            target_mode = {0x4C, 'u32'}, effect_base = {0x50, 'f32'}, effect_per_level = {0x54, 'f32'},
            effect2_base = {0x58, 'f32'}, effect2_per_level = {0x5C, 'f32'}, param_a_base = {0x60, 'i32'},
            param_a_per_level = {0x64, 'i32'}, param_b_base = {0x68, 'i32'}, param_b_per_level = {0x6C, 'i32'},
            class_mask = {0x70, 'u32'}, flags = {0x74, 'u32'},
        },
    },
    T2 = {
        native = 'ability_t2', stride = 0x94, unknown = {kind = 'u16', size = 2},
        old_named = {id = 0x00, name_text = 0x04, description_text = 0x08, target_mode = 0x3C, value0_base = 0x42, value0_per_level = 0x44,
            value1_base = 0x46, value1_per_level = 0x48, value2_base = 0x4A, value2_per_level = 0x4C, value3_base = 0x4E, value3_per_level = 0x50,
            regen_base = 0x3E, regen_per_level = 0x40},
        fields = {
            id = {0x00, 'i16'}, name_text = {0x04, 'u32'},
            description_text = {0x08, 'u32'}, icon_frame_normal = {0x30, 'i32'},
            icon_frame_hover = {0x34, 'i32'}, icon_frame_pressed = {0x38, 'i32'},
            launch_type = {0x3C, 'u8'}, target_mode = {0x3C, 'u8'},
            regen_base = {0x3E, 'u16'}, cooldown_base = {0x3E, 'u16'},
            regen_per_level = {0x40, 'u16'}, cooldown_per_level = {0x40, 'u16'}, value0_base = {0x42, 'i16'},
            damage_physical_base = {0x42, 'i16'}, damage_physical_per_level = {0x44, 'i16'}, value0_per_level = {0x44, 'i16'},
            value1_base = {0x46, 'i16'}, damage_fire_base = {0x46, 'i16'}, value1_per_level = {0x48, 'i16'},
            damage_fire_per_level = {0x48, 'i16'}, damage_magic_base = {0x4A, 'i16'}, value2_base = {0x4A, 'i16'},
            damage_magic_per_level = {0x4C, 'i16'}, value2_per_level = {0x4C, 'i16'}, value3_base = {0x4E, 'i16'},
            damage_poison_base = {0x4E, 'i16'}, value3_per_level = {0x50, 'i16'}, damage_poison_per_level = {0x50, 'i16'},
            attack_bonus_base = {0x52, 'i16'}, attack_mod_base = {0x52, 'i16'}, attack_mod_per_level = {0x54, 'i16'},
            attack_bonus_per_level = {0x54, 'i16'}, defense_mod_base = {0x56, 'i16'}, defense_bonus_base = {0x56, 'i16'},
            defense_mod_per_level = {0x58, 'i16'}, defense_bonus_per_level = {0x58, 'i16'}, res_physical_base = {0x5A, 'i16'},
            resist_physical_base = {0x5A, 'i16'}, res_physical_per_level = {0x5C, 'i16'}, resist_physical_per_level = {0x5C, 'i16'},
            resist_fire_base = {0x5E, 'i16'}, res_fire_base = {0x5E, 'i16'}, res_fire_per_level = {0x60, 'i16'},
            resist_fire_per_level = {0x60, 'i16'}, res_magic_base = {0x62, 'i16'}, resist_magic_base = {0x62, 'i16'},
            resist_magic_per_level = {0x64, 'i16'}, res_magic_per_level = {0x64, 'i16'}, res_poison_base = {0x66, 'i16'},
            resist_poison_base = {0x66, 'i16'}, res_poison_per_level = {0x68, 'i16'}, resist_poison_per_level = {0x68, 'i16'},
            speed_mod_base = {0x6A, 'i16'}, speed_bonus_base = {0x6A, 'i16'}, speed_bonus_per_level = {0x6C, 'i16'},
            speed_mod_per_level = {0x6C, 'i16'}, mental_regen_mod_base = {0x6E, 'i16'}, mental_regen_bonus_base = {0x6E, 'i16'},
            mental_regen_mod_per_level = {0x70, 'i16'}, mental_regen_bonus_per_level = {0x70, 'i16'}, physical_regen_bonus_base = {0x72, 'i16'},
            physical_regen_mod_base = {0x72, 'i16'}, physical_regen_mod_per_level = {0x74, 'i16'}, physical_regen_bonus_per_level = {0x74, 'i16'},
            value_a_base = {0x76, 'i16'}, param0_base = {0x76, 'i16'}, value_a_per_level = {0x78, 'i16'},
            param0_per_level = {0x78, 'i16'}, value_b_base = {0x7A, 'i16'}, param1_base = {0x7A, 'i16'},
            value_b_per_level = {0x7C, 'i16'}, param1_per_level = {0x7C, 'i16'}, value_c_base = {0x7E, 'i16'},
            param2_base = {0x7E, 'i16'}, value_c_per_level = {0x80, 'i16'}, param2_per_level = {0x80, 'i16'},
            value_a_cap = {0x82, 'u16'}, param0_cap = {0x82, 'u16'}, param1_cap = {0x84, 'u16'},
            value_b_cap = {0x84, 'u16'}, param2_cap = {0x86, 'u16'}, value_c_cap = {0x86, 'u16'},
            element = {0x88, 'u16'},
            class_mask = {0x8C, 'u32'}, flags = {0x90, 'u8'}, mount_flags = {0x90, 'u8'},
        },
    },
}
do
    -- the unk_<offset> slots of the first version (every aligned slot the old named fields did not cover) stay as aliases
    for _, t in pairs(ABILITY_TABLES) do
        local covered = {}
        for name, off in pairs(t.old_named) do
            for b = off, off + KIND_SIZE[t.fields[name][2]] - 1 do covered[b] = true end
        end
        local size = t.unknown.size
        for off = size, t.stride - size, size do
            local free = true
            for b = off, off + size - 1 do
                if covered[b] then free = false end
            end
            local name = ('unk_%02x'):format(off)
            if free and not t.fields[name] then t.fields[name] = {off, t.unknown.kind} end
        end
        t.old_named = nil
    end
end

ABILITY_TABLES.A1 = {native = 'ability_a1', stride = 0x78, unknown = ABILITY_TABLES.T1.unknown, fields = ABILITY_TABLES.T1.fields}
ABILITY_TABLES.A2 = {native = 'ability_a2', stride = 0x94, unknown = ABILITY_TABLES.T2.unknown, fields = ABILITY_TABLES.T2.fields}
local ability_index         -- id -> {table = 'T1'|'T2', index = n}
kernel.alias_owner = {}     -- alias id -> mod that made it (copies of abilities, gear.classes.copy_ability)
local ability_journal = {}  -- mod -> {[table:index:offset] = {native, index, offset, kind, original}}

local function build_ability_index()
    if ability_index then return ability_index end
    local index = {}
    for _, name in ipairs({'T1', 'T2'}) do
        local t = ABILITY_TABLES[name]
        local count = native.table_info(t.native)
        if not count or count == 0 then return nil end
        for i = 0, count - 1 do
            local id = native.table_read(t.native, i, 0, 'i16')
            if id and not index[id] then index[id] = {table = name, index = i} end
        end
    end
    ability_index = index
    return index
end

-- The table and record of ability `id`: the game's own, or an alias made by gear.classes.copy_ability.
function kernel.ability_entry(id)
    local index = build_ability_index()
    local e = index and index[id]
    if e then return e end
    if kernel.alias_owner[id] then
        for _, a in ipairs(native.alias_list()) do
            if a.id == id then return {table = a.table == 1 and 'A1' or 'A2', index = a.index, base = a.base} end
        end
    end
    return nil
end

local function ability_write(mod, t, index, field, value)
    local f = t.fields[field]
    local before = native.table_read(t.native, index, f[1], f[2])
    local journal = ability_journal[mod]
    if not journal then
        journal = {}
        ability_journal[mod] = journal
    end
    local key = ('%s:%d:%d'):format(t.native, index, f[1])
    if before ~= nil and not journal[key] then
        journal[key] = {t.native, index, f[1], f[2], before}
    end
    return native.table_write(t.native, index, f[1], f[2], value)
end

local ability_mt = {}
function ability_mt.__index(self, field)
    local info = rawget(self, '__info')
    if field == 'table' then return info.table end
    if field == 'index' then return info.index end
    if field == 'kind' then return (info.table == 'T1' or info.table == 'A1') and 'combat_art' or 'spell' end
    if field == 'id' then return rawget(self, '__id') end
    if field == 'base' then return info.base or rawget(self, '__id') end
    local t = ABILITY_TABLES[info.table]
    local f = t.fields[field]
    if not f then return nil end
    return native.table_read(t.native, info.index, f[1], f[2])
end
function ability_mt.__newindex(self, field, value)
    local info = rawget(self, '__info')
    local t = ABILITY_TABLES[info.table]
    local f = t.fields[field]
    if not f then
        error(("ability %d (%s) has no field '%s'"):format(rawget(self, '__id'), info.table, tostring(field)), 2)
    end
    if field == 'id' then error('the id of an ability cannot be changed', 2) end
    if type(value) ~= 'number' then error(("field '%s' needs a number"):format(field), 2) end
    if not ability_write(current or 'kernel', t, info.index, field, value) then
        error(("cannot write %s = %s (out of range for %s, or the table is not available)")
            :format(field, tostring(value), f[2]), 2)
    end
end
ability_mt.__metatable = false

local abilities = {__writable_view = true}
local abilities_mt = {}
function abilities_mt.__index(self, id)
    if type(id) ~= 'number' then return rawget(abilities, id) end
    local entry = kernel.ability_entry(id)
    if not entry then return nil end
    return setmetatable({__info = entry, __id = id}, ability_mt)
end
function abilities_mt.__newindex() error('abilities are changed field by field: gear.data.abilities[id].field = value', 2) end
abilities_mt.__metatable = false

-- The ids of all abilities, ascending.
function abilities:ids()
    local index = build_ability_index()
    local out = {}
    for id in pairs(index or {}) do out[#out + 1] = id end
    for id in pairs(kernel.alias_owner) do out[#out + 1] = id end
    table.sort(out)
    return out
end

-- {{name, offset, kind}, ...}: the fields of the ability `id`'s table, by offset; nil if there is no such ability.
function abilities:fields(id)
    local entry = kernel.ability_entry(id)
    if not entry then return nil end
    local out = {}
    for name, f in pairs(ABILITY_TABLES[entry.table].fields) do
        out[#out + 1] = {name = name, offset = f[1], kind = f[2]}
    end
    table.sort(out, function(a, b) return a.offset < b.offset end)
    return out
end

-- edits = {[id] = {field = value, ...}}: checks every id, field and value first, then writes all of them. Returns the
-- number of fields written, or nil and the list of problems (nothing is written then).
function abilities:apply(edits)
    assert(type(edits) == 'table', 'edits must be a table')
    local index = build_ability_index()
    if not index then return nil, {'the ability tables are not available'} end
    local problems, plan = {}, {}
    for id, fields in pairs(edits) do
        local entry = kernel.ability_entry(id)
        if not entry then
            problems[#problems + 1] = ('ability %s does not exist'):format(tostring(id))
        else
            local t = ABILITY_TABLES[entry.table]
            for field, value in pairs(fields) do
                local f = t.fields[field]
                if not f then
                    problems[#problems + 1] = ('ability %d (%s) has no field %s'):format(id, entry.table, tostring(field))
                elseif field == 'id' then
                    problems[#problems + 1] = ('ability %d: the id cannot be changed'):format(id)
                elseif type(value) ~= 'number' then
                    problems[#problems + 1] = ('ability %d: %s needs a number'):format(id, field)
                else
                    plan[#plan + 1] = {t, entry.index, field, value, id}
                end
            end
        end
    end
    if #problems > 0 then
        table.sort(problems)
        return nil, problems
    end
    local written = 0
    for _, p in ipairs(plan) do
        if ability_write(current or 'kernel', p[1], p[2], p[3], p[4]) then
            written = written + 1
        else
            problems[#problems + 1] = ('ability %d: %s = %s does not fit'):format(p[5], p[3], tostring(p[4]))
        end
    end
    if #problems > 0 then
        table.sort(problems)
        return written, problems
    end
    return written
end

restore_abilities = function(mod)
    local journal = ability_journal[mod]
    if not journal then return 0 end
    local n = 0
    for _, e in pairs(journal) do
        if native.table_write(e[1], e[2], e[3], e[4], e[5]) then n = n + 1 end
    end
    ability_journal[mod] = nil
    return n
end

-- Undoes this mod's writes to the ability tables; returns how many fields it restored.
function abilities:restore()
    return restore_abilities(current or 'kernel')
end

setmetatable(abilities, abilities_mt)
data.abilities = abilities

---------------------------------------------------------------------------------------------------------------
-- The region kill table (gear.data.regions): the engine's countdown per region, which brings the region boss when
-- it reaches 0 (32 records of 0x40 bytes, record 0 unused). +0x10 the name (32 bytes), +0x30 the kills a region
-- needs, +0x34 the boss flag, +0x38 the kills left, +0x3C the region id. Entries the game never set up have 0 kills
-- needed. `kills_needed` and `kills_left` can be written (whole numbers 0 .. REGION_MAX).
---------------------------------------------------------------------------------------------------------------
do
local REGION_MAX = 1000000
local REGION_FIELDS = {
    kills_needed = {0x30, 'u32', true}, boss = {0x34, 'u8'}, kills_left = {0x38, 'u32', true}, id = {0x3C, 'u8'},
}
local region_mt = {}
function region_mt.__index(self, field)
    local info = rawget(self, '__index')
    if field == 'index' then return info end
    if field == 'name' then return native.table_string('regions', info, 0x10, 32) end
    local f = REGION_FIELDS[field]
    if not f then return nil end
    local v = native.table_read('regions', info, f[1], f[2])
    if field == 'boss' and v ~= nil then return v ~= 0 end
    return v
end
function region_mt.__newindex(self, field, value)
    local f = REGION_FIELDS[field]
    if not f or not f[3] then error(("region field '%s' is read only or unknown"):format(tostring(field)), 2) end
    if type(value) ~= 'number' or value % 1 ~= 0 or value < 0 or value > REGION_MAX then
        error(("field '%s' needs a whole number from 0 to %d"):format(field, REGION_MAX), 2)
    end
    if not native.table_write('regions', rawget(self, '__index'), f[1], f[2], value) then
        error('the region table is not available', 2)
    end
end
region_mt.__metatable = false

local regions = {__writable_view = true}
function regions:count()
    local count = native.table_info('regions')
    return count or 0
end
-- The record `index` (1 .. count - 1; 0 is unused); nil if out of range or the table is not there.
function regions:get(index)
    local count = native.table_info('regions')
    if not count or type(index) ~= 'number' or index < 1 or index >= count or index % 1 ~= 0 then return nil end
    return setmetatable({__index = index}, region_mt)
end
data.regions = regions
-- The record of the region the sector `sector` (a sector word: creature:sector(), position.sector) lies in, and the
-- region byte the map files give the sector; nil if the game has no region there (or the engine is not ready).
function regions:of_sector(sector)
    if type(sector) ~= 'number' then return nil end
    local index, code = native.region_of_sector(sector)
    if not index then return nil end
    return self:get(index), code
end
-- The region a creature (a hero) is in.
function regions:of_creature(creature)
    if type(creature) ~= 'table' or type(creature.sector) ~= 'function' then return nil end
    local ok, sector = pcall(creature.sector, creature)
    if not ok then return nil end
    return self:of_sector(sector)
end
end

-- Read-only views of kernel tables: mods share `data`, `json` and the world functions, so none may change them
-- for the others. Nested tables are wrapped too (once each).
local frozen = setmetatable({}, {__mode = 'k'})
local function readonly(t)
    if type(t) ~= 'table' then return t end
    if rawget(t, '__writable_view') then return t end
    if frozen[t] then return frozen[t] end
    local proxy = setmetatable({}, {
        __index = function(_, k) return readonly(t[k]) end,
        __newindex = function() error('read only', 2) end,
        __metatable = false,
    })
    frozen[t] = proxy
    return proxy
end

---------------------------------------------------------------------------------------------------------------
-- JSON (gear.json): encode(value) -> text; decode(text) -> value | nil, message. Objects are tables with string
-- keys, arrays tables with 1..n; json.null stands for null (decode gives it for null values in arrays and objects).
---------------------------------------------------------------------------------------------------------------
local json = {null = setmetatable({}, {__tostring = function() return 'null' end, __metatable = false})}

do
    local escapes = {['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n',
                     ['\r'] = '\\r', ['\t'] = '\\t'}
    local function escape(s)
        return '"' .. s:gsub('[%c"\\]', function(c)
            return escapes[c] or string.format('\\u%04x', c:byte())
        end) .. '"'
    end

    local encode
    local function is_array(t)
        local n = 0
        for k in pairs(t) do
            if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 then return false end
            if k > n then n = k end
        end
        return n == #t, n
    end

    function encode(v, depth)
        depth = depth or 0
        if depth > 64 then error('json: nested too deeply', 0) end
        local t = type(v)
        if v == nil or v == json.null then return 'null'
        elseif t == 'boolean' then return tostring(v)
        elseif t == 'number' then
            if v ~= v or v == math.huge or v == -math.huge then error('json: cannot encode ' .. tostring(v), 0) end
            if v % 1 == 0 and v > -1e15 and v < 1e15 then return string.format('%d', v) end
            return string.format('%.17g', v)
        elseif t == 'string' then return escape(v)
        elseif t == 'table' then
            local array, n = is_array(v)
            local parts = {}
            if array and (n > 0 or next(v) == nil) then
                for i = 1, n do parts[i] = encode(v[i], depth + 1) end
                return '[' .. table.concat(parts, ',') .. ']'
            end
            local keys = {}
            for k in pairs(v) do
                if type(k) ~= 'string' then error('json: object keys must be strings', 0) end
                keys[#keys + 1] = k
            end
            table.sort(keys)
            for i, k in ipairs(keys) do parts[i] = escape(k) .. ':' .. encode(v[k], depth + 1) end
            return '{' .. table.concat(parts, ',') .. '}'
        end
        error('json: cannot encode a ' .. t, 0)
    end

    function json.encode(v)
        local ok, result = pcall(encode, v)
        if not ok then error(result, 2) end
        return result
    end

    local function utf8(cp)
        if cp < 0x80 then return string.char(cp)
        elseif cp < 0x800 then return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
        elseif cp < 0x10000 then
            return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
        end
        return string.char(0xF0 + math.floor(cp / 0x40000), 0x80 + math.floor(cp / 0x1000) % 0x40,
                           0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
    end

    function json.decode(text)
        if type(text) ~= 'string' then return nil, 'json: text must be a string' end
        local pos = 1
        local function fail(msg) error({msg = 'json: ' .. msg .. ' at ' .. pos}, 0) end
        local function ws() pos = text:find('[^ \t\r\n]', pos) or #text + 1 end
        local value
        local function str()
            pos = pos + 1
            local parts = {}
            while true do
                local a, b = text:find('[^"\\]*', pos)
                parts[#parts + 1] = text:sub(a, b)
                pos = b + 1
                local c = text:sub(pos, pos)
                if c == '"' then pos = pos + 1; return table.concat(parts) end
                if c == '' then fail('unterminated string') end
                local e = text:sub(pos + 1, pos + 1)
                if e == 'u' then
                    local hex = text:sub(pos + 2, pos + 5)
                    if not hex:match('^%x%x%x%x$') then fail('bad \\u escape') end
                    local cp = tonumber(hex, 16)
                    pos = pos + 6
                    if cp >= 0xD800 and cp < 0xDC00 and text:sub(pos, pos + 1) == '\\u' then
                        local low = tonumber(text:sub(pos + 2, pos + 5), 16)
                        if low and low >= 0xDC00 and low < 0xE000 then
                            cp = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00)
                            pos = pos + 6
                        end
                    end
                    parts[#parts + 1] = utf8(cp)
                else
                    local map = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}
                    if not map[e] then fail('bad escape') end
                    parts[#parts + 1] = map[e]
                    pos = pos + 2
                end
            end
        end
        local depth = 0
        function value()
            ws()
            local c = text:sub(pos, pos)
            if c == '{' then
                depth = depth + 1
                if depth > 64 then fail('nested too deeply') end
                pos = pos + 1
                local obj = {}
                ws()
                if text:sub(pos, pos) == '}' then pos = pos + 1; depth = depth - 1; return obj end
                while true do
                    ws()
                    if text:sub(pos, pos) ~= '"' then fail('expected a key') end
                    local k = str()
                    ws()
                    if text:sub(pos, pos) ~= ':' then fail("expected ':'") end
                    pos = pos + 1
                    obj[k] = value()
                    ws()
                    local d = text:sub(pos, pos)
                    pos = pos + 1
                    if d == '}' then depth = depth - 1; return obj end
                    if d ~= ',' then fail("expected ',' or '}'") end
                end
            elseif c == '[' then
                depth = depth + 1
                if depth > 64 then fail('nested too deeply') end
                pos = pos + 1
                local arr = {}
                ws()
                if text:sub(pos, pos) == ']' then pos = pos + 1; depth = depth - 1; return arr end
                while true do
                    arr[#arr + 1] = value()
                    ws()
                    local d = text:sub(pos, pos)
                    pos = pos + 1
                    if d == ']' then depth = depth - 1; return arr end
                    if d ~= ',' then fail("expected ',' or ']'") end
                end
            elseif c == '"' then return str()
            elseif text:sub(pos, pos + 3) == 'true' then pos = pos + 4; return true
            elseif text:sub(pos, pos + 4) == 'false' then pos = pos + 5; return false
            elseif text:sub(pos, pos + 3) == 'null' then pos = pos + 4; return json.null
            end
            local num = text:match('^-?%d+%.?%d*[eE]?[-+]?%d*', pos)
            if not num or num == '' or num == '-' then fail('unexpected character') end
            pos = pos + #num
            local n = tonumber(num)
            if not n then fail('bad number') end
            return n
        end
        local ok, result = pcall(function()
            local v = value()
            ws()
            if pos <= #text then fail('trailing text') end
            return v
        end)
        if ok then return result end
        if type(result) == 'table' then return nil, result.msg end
        return nil, 'json: ' .. tostring(result)
    end
end

---------------------------------------------------------------------------------------------------------------
-- Timers: gear.after(seconds, fn), gear.every(seconds, fn) -> timer; gear.cancel(timer). They run on the engine
-- thread before the frame handlers, in real seconds while the game runs frames (not in menus or loading screens).
---------------------------------------------------------------------------------------------------------------
local timers = {}       -- array of {mod, fn, due, period, cancelled}
local MAX_TIMERS = 1000 -- per mod
local timer_of = setmetatable({}, {__mode = 'k'})  -- the handle a mod holds -> its timer

local function add_timer(mod, seconds, fn, period)
    assert(type(seconds) == 'number' and seconds >= 0, 'seconds must be a number >= 0')
    assert(type(fn) == 'function', 'callback must be a function')
    mod.timer_count = (mod.timer_count or 0) + 1
    if mod.timer_count > MAX_TIMERS then
        mod.timer_count = mod.timer_count - 1
        error('too many timers', 3)
    end
    local t = {mod = mod, fn = fn, due = native.time() + seconds, period = period}
    timers[#timers + 1] = t
    local handle = setmetatable({}, {__metatable = false})
    timer_of[handle] = t
    return handle
end

local function run_timers()
    if #timers == 0 then return end
    local now = native.time()
    local due = {}
    local keep = {}
    for _, t in ipairs(timers) do
        if t.cancelled or t.mod.disabled then
            t.mod.timer_count = t.mod.timer_count - 1
        elseif t.due <= now then
            due[#due + 1] = t
            if t.period then
                t.due = math.max(t.due + t.period, now)
                keep[#keep + 1] = t
            else
                t.mod.timer_count = t.mod.timer_count - 1
            end
        else
            keep[#keep + 1] = t
        end
    end
    timers = keep
    for _, t in ipairs(due) do
        if not t.cancelled and not t.mod.disabled then
            local ok = call(t.mod, 'timer', t.fn)
            if not ok and t.period then t.cancelled = true end
        end
    end
end

---------------------------------------------------------------------------------------------------------------
-- Named pipes (gear.pipe.open). A mod opens only the pipes its mod.lua lists in `pipes`.
---------------------------------------------------------------------------------------------------------------
local pipes = {}        -- array of {mod, handle, name, object}

local function open_pipe(mod, name)
    assert(type(name) == 'string', 'pipe name must be a string')
    if not mod.pipes[name] then
        error('pipe ' .. name .. ' is not listed in mod.lua (pipes = {...})', 3)
    end
    local handle = native.pipe_open(name)
    if not handle then error('cannot open pipe ' .. name, 3) end
    local entry = {mod = mod, handle = handle, name = name}
    -- The pipe object: p:send(text), p:connected(), p:close(); set p.on_message(text), p.on_connect(),
    -- p.on_disconnect() to receive.
    local p = {name = name}
    function p:send(text)
        assert(type(text) == 'string', 'text must be a string')
        if entry.closed then return false end
        return native.pipe_send(handle, text)
    end
    function p:connected()
        return not entry.closed and native.pipe_connected(handle)
    end
    function p:close()
        if entry.closed then return end
        entry.closed = true
        native.pipe_close(handle)
    end
    entry.object = p
    pipes[#pipes + 1] = entry
    return p
end

local PIPE_EVENTS_PER_FRAME = 32
local function poll_pipes()
    local keep = {}
    for _, e in ipairs(pipes) do
        if not e.closed and e.mod.disabled then
            e.closed = true
            native.pipe_close(e.handle)
        end
        if not e.closed then
            keep[#keep + 1] = e
            for _, ev in ipairs(native.pipe_poll(e.handle, PIPE_EVENTS_PER_FRAME)) do
                local kind, text = ev[1], ev[2]
                local fn = (kind == 'message' and e.object.on_message) or (kind == 'connect' and e.object.on_connect)
                           or (kind == 'disconnect' and e.object.on_disconnect)
                if type(fn) == 'function' then
                    call(e.mod, 'pipe ' .. e.name .. ' ' .. kind, fn, kind == 'message' and text or nil)
                end
                if e.closed or e.mod.disabled then break end
            end
        end
    end
    pipes = keep
end

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
function creature_mt:display_name() local o = info(self); return o and data.object_types:label(o.type) end
function creature_mt:position() local o = info(self); if o then return o.x, o.y, o.layer end end
function creature_mt:sector() local o = info(self); return o and o.sector end
function creature_mt:hp() local o = info(self); return o and o.hp end
function creature_mt:max_hp() local o = info(self); return o and o.max_hp end
function creature_mt:is_dead() local o = info(self); return not o or not o.creature or o.hp <= 0 end
function creature_mt:gold() local o = info(self); return o and o.gold end
function creature_mt:level() local o = info(self); return o and o.level end
function creature_mt:experience() local o = info(self); return o and o.exp end
function creature_mt:set_hp(hp)
    assert(type(hp) == 'number', 'hp must be a number')
    return native.set_health(self.id, math.floor(hp))
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

    -- The image id (for gear.ui.image) of an ability's icon: the game's own texture, 64 x 64; nil if it has none. Copies use their
    -- base's icon (a copy's record is a copy of the base's).
    function C.art_icon(id)
        local name = C.icon_name(id)
        local image = name and native.texture_image(name) or nil
        if not image then return nil end
        -- the game's icons are grey discs; the combat arts' are drawn gold, the spells' a pale blue
        return image, id >= 1000 and 0xFFFFD060 or 0xFFB8D0FF
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
    -- The stats block's float fields cStats::recalc works out (see gear/buffs.h), by name: sums (0 before) and
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
    -- The spell overlays the game draws a hero with (creature +0x60).
    B.OVERLAYS = {light = 6, stone = 7, flame = 10}
    B.defs = {}
    B.order = {}
    B.meta = {}

    local function offset_of(name, factor)
        if type(name) == 'number' then return name end
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
        local overlay = def.overlay
        if type(overlay) == 'string' then overlay = assert(B.OVERLAYS[overlay], 'unknown overlay ' .. overlay) end
        local d = {id = id, name = def.name or def.id, description = def.description, icon = def.icon,
            stats = def.stats or {}, factors = def.factors or {}, overlay = overlay or 0, seconds = def.seconds or 0,
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
    function creature_mt:forget(ability)
        assert(type(ability) == 'number', 'expected an ability id')
        return native.creature_forget(self.id, ability)
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
    -- share_experience(share, radius): experience a party hero gains from the game also goes to the others within
    -- `radius` world units (0 or nil: anywhere), times `share` (0 turns it off, 1 the full amount).
    function P.share_experience(share, radius)
        assert(type(share) == 'number', 'share must be a number')
        native.party_share(share, radius or 0)
    end
end

-- Multiplayer (gear.mp). A player is a hero object: slots 1..16 are the heroes' object ids. See docs/GearMultiplayer.md.
local mp = {}
do
local player_mt = {}
player_mt.__index = player_mt
-- native.mp_info() -> {network, is_host, host_slot} (host_slot nil while it is not known).
local function mp_info()
    return native.mp_info and native.mp_info() or {network = false}
end
local function host_slot()
    local i = mp_info()
    if not i.network or i.is_host then return native.hero() or (not i.network and 1 or nil) end
    return i.host_slot
end
local function make_player(id)
    local o = native.object(id)
    if not (o and o.creature) then return nil end
    local cname = creature_mt.class({id = id})
    local name = native.hero_name and native.hero_name(id) or nil
    local p = {slot = id, class = cname, is_local = native.hero() == id, name = name or ('hero ' .. id)}
    p.key = p.name:lower() .. ':' .. (cname or '?')
    p.is_host = host_slot() == id
    return setmetatable(p, player_mt)
end
function player_mt:hero() return wrap(self.slot) end
-- A hero's name as the game shows it (nil for other creatures).
function creature_mt:name()
    if self.id < 1 or self.id > 16 then return nil end
    return native.hero_name and native.hero_name(self.id) or nil
end
function mp.players()
    local out = {}
    for id = 1, 16 do
        local p = make_player(id)
        if p then out[#out + 1] = p end
    end
    return out
end
function mp.player(slot)
    assert(type(slot) == 'number', 'slot must be a number')
    if slot < 1 or slot > 16 then return nil end
    return make_player(math.floor(slot))
end
function mp.local_player()
    local id = native.hero()
    return id and make_player(id) or nil
end
-- Whether other players' heroes are in the game (a network game with more than the local player).
function mp.active() return #mp.players() > 1 end
-- mp.mode() -> 'single' | 'lan' | 'open' | 'closed': the kind of game, from the engine's own flags (see World::networkMode).
function mp.mode() return native.mp_mode() end
function mp.local_slot() return native.hero() or (not mp_info().network and 1 or nil) end
function mp.host_slot() return host_slot() end
function mp.is_host()
    local i = mp_info()
    return not i.network or i.is_host == true
end

-- The mod channel. A message (kind 's' string or 'j' JSON, the channel's length and name, the body) is cut into frames
-- the native side sends as private chat (0x003) to the target slot: native.mp_send_frame(slot, utf16le) -> bool;
-- received frames come from native.mp_take_frame() -> slot, utf16le. A frame is UTF-16 units: the marker, the message
-- number, the fragment index, in the first fragment the total length (two units), then the bytes packed 15 bits per
-- unit; every unit but the marker is 0x4000 + a 15-bit value (no NUL, no surrogates). At most 127 units (the 254 bytes
-- the server relays): 228 bytes in the first frame, 232 in each further one.
local C = {
    MARKER = 0xE1A0, FIRST = 228, NEXT = 232, MAX_BODY = 65536, RATE = 8192, BURST = 2048, PER_TICK = 4,
    MAX_QUEUED = 512 * 1024, EXPIRE = 30, MAX_PARTIAL = 16,
    queue = {}, qhead = 1, qtail = 0, queued = 0, credit = 2048, number = 0, now = 0,
    loop = {}, partial = {}, force = false,
}
local N   -- the custom net messages (set below)
mp._channel = C   -- for the kernel tests (names starting with _ are not part of the API)

local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift
local schar, sbyte = string.char, string.byte

local function units_to_le(units)
    local parts = {}
    for i = 1, #units do
        local u = units[i]
        parts[i] = schar(band(u, 0xFF), rshift(u, 8))
    end
    return table.concat(parts)
end
-- The bytes s[i .. j] packed 15 bits per unit, appended to `units`.
local function pack(s, i, j, units)
    local acc, n = 0, 0
    for k = i, j do
        acc = bor(lshift(acc, 8), sbyte(s, k))
        n = n + 8
        if n >= 15 then
            n = n - 15
            units[#units + 1] = 0x4000 + band(rshift(acc, n), 0x7FFF)
            acc = band(acc, lshift(1, n) - 1)
        end
    end
    if n > 0 then units[#units + 1] = 0x4000 + band(lshift(acc, 15 - n), 0x7FFF) end
end
-- `count` bytes from units[from ..], appended to `out`; false if the units run out first.
local function unpack_units(units, from, count, out)
    local acc, n, got = 0, 0, 0
    for k = from, #units do
        if got >= count then break end
        acc = bor(lshift(acc, 15), units[k] - 0x4000)
        n = n + 15
        while n >= 8 and got < count do
            n = n - 8
            got = got + 1
            out[#out + 1] = schar(band(rshift(acc, n), 0xFF))
        end
        acc = band(acc, lshift(1, n) - 1)
    end
    return got == count
end
function C.frame_count(len) return len <= C.FIRST and 1 or 1 + math.ceil((len - C.FIRST) / C.NEXT) end
-- The frames (UTF-16LE strings) of one encoded message.
function C.frames(data, number)
    local out, len, pos = {}, #data, 1
    for index = 0, C.frame_count(len) - 1 do
        local units = {C.MARKER, 0x4000 + number, 0x4000 + index}
        local size = index == 0 and C.FIRST or C.NEXT
        if index == 0 then
            units[4] = 0x4000 + rshift(len, 15)
            units[5] = 0x4000 + band(len, 0x7FFF)
        end
        pack(data, pos, math.min(pos + size - 1, len), units)
        pos = pos + size
        out[#out + 1] = units_to_le(units)
    end
    return out
end
function C.encode(channel, payload)
    if type(channel) ~= 'string' or not channel:match('^[%w_%.%-]+$') or #channel > 32 then
        return nil, 'channel must be 1 to 32 letters, digits, _ . -'
    end
    local kind, body = 's', payload
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        kind, body = 'j', text
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    if #body > C.MAX_BODY then return nil, 'the message is larger than 64 KB' end
    return kind .. schar(#channel) .. channel .. body
end
function C.decode(data)
    local kind, n = data:sub(1, 1), sbyte(data, 2)
    if not n or n == 0 or #data < 2 + n then return nil end
    local channel, body = data:sub(3, 2 + n), data:sub(3 + n)
    -- s/j: gear.mp messages; n/k: gear.net channel messages on the chat carrier (string / JSON); q/a: the net handshake
    if kind == 's' or kind == 'n' or kind == 'q' or kind == 'a' then return channel, body, kind end
    if kind == 'j' or kind == 'k' then
        local v = json.decode(body)
        if v ~= nil then return channel, v, kind end
    end
end

function C.deliver(slot, data)
    local channel, payload, kind = C.decode(data)
    if not channel then
        log('gear.mp: a message from slot %s could not be read', tostring(slot))
        return
    end
    if kind == 'n' or kind == 'k' then return N.chat_message(slot, channel, payload) end
    if kind == 'q' or kind == 'a' then return N.chat_control(slot, kind, payload) end
    kernel.dispatch('mp_message', C.player_of(slot), channel, payload)
end

-- gear.mp.send(channel, payload, to) -> true | nil, message. `to`: a slot, a player, 'host' (default) or 'all'.
function mp.send(channel, payload, to)
    local data, err = C.encode(channel, payload)
    if not data then return nil, err end
    local me = native.hero()
    if not me then return nil, 'no game is running' end
    local network = mp_info().network
    if to == nil then to = 'host' end
    if type(to) == 'table' then to = to.slot end
    local targets = {}
    if to == 'host' then
        local h = host_slot()
        if not h then return nil, 'the host is not known yet' end
        targets[1] = h
    elseif to == 'all' then
        if network then
            for _, p in ipairs(mp.players()) do targets[#targets + 1] = p.slot end
        else
            targets[1] = me
        end
    elseif type(to) == 'number' and to >= 1 and to <= 16 and to == math.floor(to) then
        if to ~= me and not (network and mp.player(to)) then return nil, 'no such player' end
        targets[1] = to
    else
        return nil, "to must be a slot (1 to 16), a player, 'host' or 'all'"
    end
    local frames
    local waiting = 0
    for _, slot in ipairs(targets) do
        if slot ~= me or (C.force and network) then waiting = waiting + C.frame_count(#data) * 254 end
    end
    if C.queued + waiting > C.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    C.number = (C.number + 1) % 0x8000
    for _, slot in ipairs(targets) do
        if slot == me and not (C.force and network) then
            C.loop[#C.loop + 1] = data
        elseif slot ~= me and N.state == 'ready' and N.api.peer_supported(slot) and N.enqueue(N.mp_channel, data, 0x10 + slot) then
            C.via_net = (C.via_net or 0) + 1   -- the custom opcode carries it
        else
            frames = frames or C.frames(data, C.number)
            for _, f in ipairs(frames) do
                C.qtail = C.qtail + 1
                C.queue[C.qtail] = {slot, f}
                C.queued = C.queued + #f
            end
        end
    end
    return true
end
-- Debug: in a network game, messages to the local slot go through the real chat carrier instead of the loopback.
function mp.force_network(on) C.force = on and true or false end

function C.receive(slot, s)
    local units = {}
    for i = 1, #s - 1, 2 do units[#units + 1] = sbyte(s, i) + sbyte(s, i + 1) * 256 end
    if units[1] ~= C.MARKER or #units < 4 or #units > 127 then return end
    for i = 2, #units do
        if units[i] < 0x4000 or units[i] > 0xBFFF then return end
    end
    C.received = (C.received or 0) + 1
    local number, index = units[2] - 0x4000, units[3] - 0x4000
    local key = slot * 0x8000 + number
    local m = C.partial[key]
    if not m then
        local open, oldest = 0, nil
        for k, v in pairs(C.partial) do
            if v.slot == slot then
                open = open + 1
                if not oldest or v.at < C.partial[oldest].at then oldest = k end
            end
        end
        if open >= C.MAX_PARTIAL then C.partial[oldest] = nil end
        m = {slot = slot, at = C.now, parts = {}, got = 0}
        C.partial[key] = m
    end
    if index == 0 then
        if #units < 6 then C.partial[key] = nil return end
        m.len = (units[4] - 0x4000) * 0x8000 + (units[5] - 0x4000)
        if m.len > C.MAX_BODY + 64 then C.partial[key] = nil return end
        m.count = C.frame_count(m.len)
    end
    if not m.parts[index] then
        m.parts[index] = units
        m.got = m.got + 1
    end
    if m.count and index >= m.count then C.partial[key] = nil return end
    if not (m.count and m.got >= m.count) then return end
    C.partial[key] = nil
    local out = {}
    for i = 0, m.count - 1 do
        local first = i == 0
        local size = first and math.min(m.len, C.FIRST) or math.min(m.len - C.FIRST - (i - 1) * C.NEXT, C.NEXT)
        if not unpack_units(m.parts[i], first and 6 or 4, size, out) then return end
    end
    C.deliver(slot, table.concat(out))
end

-- Net channels (gear.net, docs/GearMultiplayer.md): channels with a name. Their messages ride on the chat carrier (frames
-- of kind n/k, to a player slot or to slot 0, the game server) and, when the server agrees, on two message types above
-- the stock range. The handshake is on the chat carrier too: the client announces itself ('h') on entering the game, the server
-- answers with a query 'q' with the numbers it offers, the client answers 'a' (accepts or declines), and on an accept sends HELLO on the agreed control type;
-- the server's HELLO_ACK (limits) and PEERS (who else can) complete it. A server that never queries (the original
-- GameServer.exe, other servers) never sees a custom type from us: channels stay on chat (to players; not to the server).
-- Native side: native.net_ops() -> control, data, ready; net_configure(control, data); net_send_op(type, bytes) -> bool;
-- net_take_op() -> type, sender slot, bytes | nil. Data frame: [u32 channel id (FNV-1a of the name)][u8 flags: 1 first,
-- 2 last, 4 JSON][u8 target: 0 server, 1 host, 0xFF all, 0x10 + slot][u16 message id][bytes], all little endian.
N = {
    control = 0x1E0, data = 0x1E1, state = 'idle', hello_at = 0, mask = 0, ack = nil, now = 0,
    FRAGMENT = 4000, MAX_MESSAGE = 262144, RATE = 65536, INFLIGHT = 8, HELLO_TIMEOUT = 3, EXPIRE = 10,
    MAX_PARTIAL_BYTES = 1048576, MAX_QUEUED = 1048576, PER_TICK = 16,
    channels = {}, partial = {}, partial_bytes = 0, queue = {}, qhead = 1, qtail = 0, queued = 0, credit = 0, number = 0,
    unknown = 0, dropped = 0, received = 0, sent = 0, supported = false, snapshot = '', server_chat = false, announces = 0, announce_at = 0,
}
mp._net = N   -- for the kernel tests
local net_api = {}
N.api = net_api
local channel_mt = {}
channel_mt.__index = channel_mt

local function fnv1a(s)
    local M = 4294967296
    local h = 0x811C9DC5
    for i = 1, #s do
        h = bit.bxor(h, sbyte(s, i)) % M
        h = (bit.lshift(h, 24) % M + h * 0x193) % M      -- h * 0x01000193 mod 2^32: the prime is 2^24 + 0x193
    end
    return h
end
N.fnv1a = fnv1a
local function u16(s, i) local a, b = sbyte(s, i, i + 1); return a + b * 256 end
local function u32(s, i) local a, b, c, d = sbyte(s, i, i + 3); return a + b * 256 + c * 65536 + d * 16777216 end
local function p16(v) return schar(band(v, 255), band(rshift(v, 8), 255)) end
local function p32(v) return schar(band(v, 255), band(rshift(v, 8), 255), band(rshift(v, 16), 255), band(rshift(v, 24), 255)) end

local function player_of(slot)
    return slot == 0 and {slot = 0, name = 'server', key = 'server', is_local = false, is_host = false}
        or mp.player(slot)
        or setmetatable({slot = slot, name = 'hero ' .. slot, key = '?', is_local = false, is_host = host_slot() == slot}, player_mt)
end
C.player_of = player_of

local function caps_changed()
    local peers = {}
    for slot = 1, 16 do if band(rshift(N.mask, slot - 1), 1) == 1 then peers[#peers + 1] = slot end end
    local supported = N.state == 'ready'
    local snapshot = (supported and table.concat(peers, ',') or '') .. (N.server_chat and '+chat' or '')
    if supported ~= N.supported or snapshot ~= N.snapshot then
        N.supported, N.snapshot = supported, snapshot
        kernel.dispatch('net_caps', {supported = supported, peers = peers, chat = N.server_chat})
    end
end

local function net_reset()
    N.state, N.mask, N.ack, N.server_chat = 'idle', 0, nil, false
    N.announces, N.announce_at = 0, 0
    N.partial, N.partial_bytes = {}, 0
    N.queue, N.qhead, N.qtail, N.queued = {}, 1, 0, 0
    caps_changed()
end

-- gear.net.configure({control = 0x1E0, data = 0x1E1}): the custom message types (for servers with opcodes of their own);
-- before the game connects. The data type is told by the server in the handshake as well.
function net_api.configure(opts)
    if type(opts) ~= 'table' then return nil, 'opts must be a table' end
    if N.state ~= 'idle' then return nil, 'the handshake has already started' end
    local c, d = opts.control or N.control, opts.data or N.data
    if not (native.net_configure and native.net_configure(c, d)) then return nil, 'invalid message types (above 0x1D5, below 0x400, different)' end
    N.control, N.data = c, d
    N.preferred = {c, d}   -- the handshake accepts only these numbers from a server
    return true
end

function net_api.supported() return N.state == 'ready' end
function net_api.peer_supported(slot)
    if N.state ~= 'ready' or type(slot) ~= 'number' or slot < 1 or slot > 16 or slot ~= math.floor(slot) then return false end
    return band(rshift(N.mask, slot - 1), 1) == 1
end
function net_api.limits()
    if N.state ~= 'ready' then return nil end
    return {max_fragment = N.FRAGMENT, max_message = N.MAX_MESSAGE, bytes_per_second = N.RATE, in_flight = N.INFLIGHT}
end

function net_api.channel(name, opts)
    if type(name) ~= 'string' or #name < 1 or #name > 64 or name:find('[^\32-\126]') then
        return nil, 'a channel name is 1 to 64 printable ASCII characters'
    end
    local id = fnv1a(name)
    local c = N.channels[id]
    if c then
        if c.name ~= name then return nil, ('channel %q has the same id as %q: rename one'):format(name, c.name) end
        return c
    end
    c = setmetatable({name = name, id = id, handlers = {}}, channel_mt)
    N.channels[id] = c
    return c
end

-- ch:on(fn): fn(from, payload) for every message of the channel; fn is returned (give it to ch:off).
function channel_mt:on(fn)
    assert(type(fn) == 'function', 'fn must be a function')
    self.handlers[#self.handlers + 1] = fn
    return fn
end
function channel_mt:off(fn)
    for i, h in ipairs(self.handlers) do
        if h == fn then table.remove(self.handlers, i) return true end
    end
    return false
end

-- Queues one message (string, or a table as JSON) as fragments; `target` is the wire byte. true | nil, message.
function N.enqueue(c, payload, target)
    if N.state ~= 'ready' then return nil, 'custom net messages are not available here (the server does not support them)' end
    local flags, body = 0, payload
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        flags, body = 4, text
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    if #body > N.MAX_MESSAGE then return nil, ('the message is larger than %d bytes'):format(N.MAX_MESSAGE) end
    if N.queued + #body > N.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    N.number = (N.number + 1) % 65536
    N.qtail = N.qtail + 1
    N.queue[N.qtail] = {c = c, flags = flags, target = target, id = N.number, body = body, pos = 1}
    N.queued = N.queued + #body
    return true
end

-- One frame group of the chat carrier: kind, name and body as one message to `slot` (0: the game server).
local function chat_frame(slot, kind, name, body)
    if #body > C.MAX_BODY then return nil, 'the message is larger than 64 KB on the chat carrier' end
    local data = kind .. schar(#name) .. name .. body
    if C.queued + C.frame_count(#data) * 254 > C.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    C.number = (C.number + 1) % 0x8000
    for _, f in ipairs(C.frames(data, C.number)) do
        C.qtail = C.qtail + 1
        C.queue[C.qtail] = {slot, f}
        C.queued = C.queued + #f
        C.last_entry = C.queue[C.qtail]
    end
    return true
end
-- A message on a net channel through the chat carrier (n: bytes, k: JSON).
function N.chat_send(slot, name, payload)
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        return chat_frame(slot, 'k', name, text)
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    return chat_frame(slot, 'n', name, payload)
end

-- How a message to `to` would travel: a list of steps {via = 'custom' | 'chat', slot, target}, or nil and why.
local function plan(to)
    if to == nil then to = 'host' end
    if type(to) == 'table' then to = to.slot end
    local me = native.hero()
    if to == 'server' then
        if N.state == 'ready' then return {{via = 'custom', slot = 0, target = 0}} end
        if N.server_chat then return {{via = 'chat', slot = 0}} end
        return nil, 'the server does not support net channels'
    end
    local slots
    if to == 'host' then
        local h = host_slot()
        if not h then return nil, 'the host is not known yet' end
        if h == me then return nil, 'a net message cannot go to this computer' end
        slots = {h}
    elseif to == 'all' then
        slots = {}
        for _, p in ipairs(mp.players()) do if p.slot ~= me then slots[#slots + 1] = p.slot end end
    elseif type(to) == 'number' and to >= 1 and to <= 16 and to == math.floor(to) then
        if to == me then return nil, 'a net message cannot go to this computer' end
        slots = {to}
    else
        return nil, "to must be 'server', 'host', 'all', a slot (1 to 16) or a player"
    end
    local custom, every = N.state == 'ready', N.state == 'ready'
    for _, s in ipairs(slots) do
        if not (custom and net_api.peer_supported(s)) then every = false end
    end
    if to == 'all' and every and #slots > 0 then return {{via = 'custom', target = 0xFF}} end
    local steps = {}
    for _, s in ipairs(slots) do
        if custom and net_api.peer_supported(s) then steps[#steps + 1] = {via = 'custom', slot = s, target = to == 'host' and 1 or 0x10 + s}
        else steps[#steps + 1] = {via = 'chat', slot = s} end
    end
    return steps
end

-- ch:send(payload, to) -> true | nil, message. `to`: 'server', 'host' (default), 'all', a slot or a player. Over the custom
-- message types where both sides support them, over the chat carrier otherwise.
function channel_mt:send(payload, to)
    local steps, err = plan(to)
    if not steps then return nil, err end
    for i, st in ipairs(steps) do
        local ok, e
        if st.via == 'custom' then ok, e = N.enqueue(self, payload, st.target) else ok, e = N.chat_send(st.slot, self.name, payload) end
        if not ok and i == 1 then return nil, e end
    end
    return true
end

-- 'custom', 'chat', 'mixed' (some players one way, some the other) or nil (not possible): the transport a message to `to`
-- would use now.
function net_api.transport(to)
    local steps = plan(to)
    if not steps or #steps == 0 then return nil end
    local custom = 0
    for _, st in ipairs(steps) do if st.via == 'custom' then custom = custom + 1 end end
    return custom == #steps and 'custom' or custom == 0 and 'chat' or 'mixed'
end
function channel_mt:transport(to) return net_api.transport(to) end

local function net_deliver(ch, slot, json_flag, body)
    local payload = body
    if json_flag then
        payload = json.decode(body)
        if payload == nil then N.dropped = N.dropped + 1 return end
    end
    N.dispatch_message(ch, slot, payload)
end
function N.dispatch_message(ch, slot, payload)
    local from = player_of(slot)
    for _, fn in ipairs({unpack(ch.handlers)}) do
        local ok, err = pcall(fn, from, payload)
        if not ok then log('gear.net: handler of %s failed: %s', ch.name, tostring(err)) end
    end
end

-- A channel message that came on the chat carrier (payload already a string or a table).
function N.chat_message(slot, name, payload)
    local ch = N.channels[fnv1a(name)]
    if not ch or ch.name ~= name then N.unknown = N.unknown + 1 return end
    N.received = N.received + 1
    N.dispatch_message(ch, slot, payload)
end

-- The handshake on the chat carrier: only the server's query 'q' [u8 proto][u16 control][u16 data] is acted on; the answer 'a'
-- [u8 proto][u16 control][u16 data][u8 flags: 1 accepts the custom types] goes back over chat, then HELLO follows on the
-- agreed control type once the answer has left (poll).
function N.chat_control(slot, kind, body)
    if slot ~= 0 or kind ~= 'q' or #body < 5 or N.state ~= 'idle' then return end
    local proto, control, data = sbyte(body, 1), u16(body, 2), u16(body, 4)
    N.server_chat = true
    local accept = proto >= 1 and native.net_configure ~= nil and (not N.preferred or (N.preferred[1] == control and N.preferred[2] == data))
        and native.net_configure(control, data) == true
    if accept then N.control, N.data = control, data end
    chat_frame(0, 'a', 'gear.net', schar(1) .. p16(N.control) .. p16(N.data) .. schar(accept and 1 or 0))
    if accept then N.state, N.hello_wait = 'answered', C.last_entry else N.state = 'chat' end
    log('gear.net: the server offered custom message types %X/%X: %s', control, data, accept and 'accepted' or 'declined')
    caps_changed()
end

local function drop_partial(key)
    local m = N.partial[key]
    if m then N.partial_bytes = N.partial_bytes - m.size; N.partial[key] = nil end
end

local function net_receive(slot, s)
    if #s < 8 then return end
    local ch = N.channels[u32(s, 1)]
    if not ch then N.unknown = N.unknown + 1 return end
    local flags, id = sbyte(s, 5), u16(s, 7)
    local first, last, js = band(flags, 1) ~= 0, band(flags, 2) ~= 0, band(flags, 4) ~= 0
    local body = s:sub(9)
    N.received = N.received + 1
    if first and last then
        if #body <= N.MAX_MESSAGE then net_deliver(ch, slot, js, body) end
        return
    end
    local key = slot * 65536 + id
    local m = N.partial[key]
    if first then
        drop_partial(key)
        local open, oldest = 0, nil
        for k, v in pairs(N.partial) do
            if v.slot == slot then
                open = open + 1
                if not oldest or v.at < N.partial[oldest].at then oldest = k end
            end
        end
        if open >= N.INFLIGHT then drop_partial(oldest) end
        m = {slot = slot, ch = ch, json = js, parts = {}, size = 0, at = N.now}
        N.partial[key] = m
    elseif not m or m.ch ~= ch then
        return
    end
    m.size = m.size + #body
    N.partial_bytes = N.partial_bytes + #body
    m.parts[#m.parts + 1] = body
    m.at = N.now
    if m.size > N.MAX_MESSAGE or N.partial_bytes > N.MAX_PARTIAL_BYTES then drop_partial(key) N.dropped = N.dropped + 1 return end
    if last then
        N.partial[key] = nil
        N.partial_bytes = N.partial_bytes - m.size
        net_deliver(ch, slot, m.json, table.concat(m.parts))
    end
end
N.receive = net_receive

local function net_control(s)
    local kind = sbyte(s, 1)
    if kind == 2 and #s >= 19 and (N.state == 'hello' or N.state == 'ready') then
        N.ack = {protocol = sbyte(s, 2), features = u32(s, 3), data = u16(s, 7), fragment = u16(s, 9), message = u32(s, 11),
            rate = u32(s, 15), in_flight = sbyte(s, 19)}
        local a = N.ack
        N.FRAGMENT = math.max(64, math.min(a.fragment, 4000))
        N.MAX_MESSAGE = a.message
        N.RATE = math.max(1024, a.rate)
        N.INFLIGHT = math.max(1, a.in_flight)
        if a.data ~= N.data and native.net_configure and native.net_configure(N.control, a.data) then N.data = a.data end
        N.state = 'ready'
        N.credit = N.RATE
        log('gear.net: the server supports custom net messages (fragment %d, message %d, %d B/s)', N.FRAGMENT, N.MAX_MESSAGE, N.RATE)
        caps_changed()
    elseif kind == 3 and #s >= 3 and N.state == 'ready' then
        N.mask = u16(s, 2)
        caps_changed()
    end
end

-- Every tick: the handshake, frames in, frames out (paced to the server's announced rate), stale partial messages.
function N.poll(dt)
    N.now = N.now + (dt or 0)
    if not native.net_ops then return end
    local _, _, ready = native.net_ops()
    local info = mp_info()
    if not ready then
        if N.state ~= 'idle' or N.server_chat then net_reset() end
        return
    end
    -- A Gear client announces itself to the server over chat on entering the game (private chat to slot 0, harmless to servers that
    -- do not know it); only a server that heard it writes channel frames to this client, which plain clients would show as chat.
    if N.state == 'idle' and not N.server_chat and info.network and native.hero() and N.announces < 6 and N.now >= N.announce_at then
        N.announces, N.announce_at = N.announces + 1, N.now + 5
        chat_frame(0, 'h', 'gear.net', schar(1))
    end
    if N.state == 'answered' and (not N.hello_wait or N.hello_wait.sent) then
        local hash = kernel.mod_set_hash and tonumber(kernel.mod_set_hash(), 16) or 0
        if native.net_send_op(N.control, schar(1, 1) .. p32(3) .. p32(hash)) then N.state, N.hello_at = 'hello', N.now end
    elseif N.state == 'hello' and N.now - N.hello_at > N.HELLO_TIMEOUT then
        N.state = 'unsupported'
        log('gear.net: no answer to HELLO: the custom message types stay off, the chat carrier is used')
    end
    for _ = 1, 64 do
        local t, slot, s = native.net_take_op()
        if not t then break end
        if t == N.control then net_control(s) elseif t == N.data and N.state == 'ready' then net_receive(slot, s) end
    end
    for k, m in pairs(N.partial) do
        if N.now - m.at > N.EXPIRE then drop_partial(k) end
    end
    if N.state ~= 'ready' then return end
    N.credit = math.min(N.credit + N.RATE * (dt or 0), N.RATE * 2)
    local sent = 0
    while N.qhead <= N.qtail and sent < N.PER_TICK and N.credit >= 0 do
        local m = N.queue[N.qhead]
        local len = #m.body
        local take = math.min(N.FRAGMENT, len - m.pos + 1)
        local flags = m.flags + (m.pos == 1 and 1 or 0) + (m.pos + take > len and 2 or 0)
        local frame = p32(m.c.id) .. schar(flags, m.target) .. p16(m.id) .. m.body:sub(m.pos, m.pos + take - 1)
        if not native.net_send_op(N.data, frame) then N.dropped = N.dropped + 1 else N.sent = N.sent + 1 end
        N.credit = N.credit - #frame - 20
        sent = sent + 1
        m.pos = m.pos + take
        if m.pos > len then
            N.queue[N.qhead] = nil
            N.qhead = N.qhead + 1
            N.queued = N.queued - len
        end
    end
    if N.qhead > N.qtail then N.queue, N.qhead, N.qtail = {}, 1, 0 end
end

-- gear.mp.send travels on this channel when the target can use it: the encoded message as raw bytes
N.mp_channel = net_api.channel('gear.mp')
N.mp_channel:on(function(from, data) C.deliver(from.slot, data) end)

-- Every tick: the loopback messages of the last tick, the frames that came in, the frames to send (paced: about
-- 8 KB/s and at most 4 frames a tick, so the game's send queue never floods).
function kernel.poll_mp(dt)
    N.poll(dt)
    C.now = C.now + (dt or 0)
    if #C.loop > 0 then
        local list = C.loop
        C.loop = {}
        local me = native.hero()
        for _, data in ipairs(list) do C.deliver(me, data) end
    end
    if native.mp_take_frame then
        for _ = 1, 64 do
            local slot, s = native.mp_take_frame()
            if not slot then break end
            C.receive(slot, s)
        end
    end
    for k, m in pairs(C.partial) do
        if C.now - m.at > C.EXPIRE then C.partial[k] = nil end
    end
    C.credit = math.min(C.credit + C.RATE * (dt or 0), C.BURST)
    local sent = 0
    while C.qhead <= C.qtail and sent < C.PER_TICK and C.credit >= 0 do
        local f = C.queue[C.qhead]
        f.sent = true
        -- false: the game cannot send it (the player left, the connection is gone): the frame is dropped
        if not (native.mp_send_frame and native.mp_send_frame(f[1], f[2])) then C.dropped = (C.dropped or 0) + 1 end
        C.queue[C.qhead] = nil
        C.qhead = C.qhead + 1
        C.queued = C.queued - #f[2]
        C.credit = C.credit - #f[2] - 20
        sent = sent + 1
    end
    if C.qhead > C.qtail then C.queue, C.qhead, C.qtail = {}, 1, 0 end
end
end

-- Items. The kind (the type record's +0x2E) -> category; quality from the rarity (as the label colours).
local ITEM_CATEGORY = {
    [5] = 'weapon', [0x21] = 'weapon', [0xD] = 'shield', [8] = 'jewelry', [0x14] = 'jewelry', [9] = 'potion',
    [0x1C] = 'rune', [0x10] = 'scroll', [7] = 'special', [0xF] = 'special', [0x1D] = 'special', [0x1E] = 'special',
    [0x1F] = 'special', [6] = 'armor', [0x11] = 'armor', [0x12] = 'armor', [0x13] = 'armor', [0x15] = 'armor',
    [0x16] = 'armor', [0x17] = 'armor', [0x18] = 'armor', [0x19] = 'armor',
}
local QUALITIES = {'normal', 'magic', 'rare', 'set', 'unique'}   -- ascending
local QUALITY_RANK = {}
for i, q in ipairs(QUALITIES) do QUALITY_RANK[q] = i end
world.QUALITIES = QUALITIES

local function quality_of(rarity, set)
    if set then return 'set' end
    if rarity >= 15 then return 'unique' end
    if rarity >= 12 then return 'rare' end
    if rarity >= 5 then return 'magic' end
    return 'normal'
end

-- Potions by type id (their type names are only model files: BOTTLE01.GRN, potion_large.grn ...): the three
-- sizes of health (0x13C0..), mana (0x13C3..) and stamina (0x13C6..) potions, and the coloured potions in three
-- sizes of five colours each (black, blue, green, red, yellow).
local POTION_COLORS = {'black', 'blue', 'green', 'red', 'yellow'}
local function potion_kind(type_id)
    if type_id >= 0x13C0 and type_id <= 0x13C2 then return 'health' end
    if type_id >= 0x13C3 and type_id <= 0x13C5 then return 'mana' end
    if type_id >= 0x13C6 and type_id <= 0x13C8 then return 'stamina' end
    for _, first in ipairs({0x1411, 0x1416, 0x1430}) do
        if type_id >= first and type_id < first + 5 then return 'colored', POTION_COLORS[type_id - first + 1] end
    end
    return 'other'
end

-- world.item(id) -> {id, type, type_name, kind, category, rarity, quality, quality_rank, set, classes (names; empty:
-- every class), class_mask, gold, quest, potion ('health', 'mana', 'stamina', 'colored', 'other') and potion_color,
-- on_ground, x, y, layer} | nil
function world.item(id)
    local it = type(id) == 'number' and native.item(id)
    if not it then return nil end
    it.type_name = data.object_types:name(it.type)
    it.category = it.gold and 'gold' or ITEM_CATEGORY[it.kind] or 'other'
    it.quality = quality_of(it.rarity, it.set)
    it.quality_rank = QUALITY_RANK[it.quality]
    it.class_mask = it.classes
    local classes = {}
    if it.class_mask ~= 0 and bit.band(it.class_mask, CLASS_INFO.all) ~= CLASS_INFO.all then
        for _, c in ipairs(CLASSES) do
            if bit.band(it.class_mask, c.bit) ~= 0 then classes[#classes + 1] = c.name end
        end
    end
    it.classes = classes
    it.quest = bit.band(it.type_flags, 0x1000000) ~= 0
    if it.category == 'potion' then it.potion, it.potion_color = potion_kind(it.type) end
    return it
end

-- The game's item label colours by quality (ENG 0x5C13C0): rarity 0-4 white, 5-11 blue, 12-14 yellow, 15 gold, set green.
world.QUALITY_COLORS = {normal = 0xFFEFEFEF, magic = 0xFFA0A0FF, rare = 0xFFF0E040, unique = 0xFFC19A33, set = 0xFF15FF25}

-- world.quality_color(quality | rarity, set) -> ARGB: a quality name or a rarity 0..15 (with `set` for set items).
function world.quality_color(q, set)
    if type(q) == 'number' then q = quality_of(q, set) end
    return world.QUALITY_COLORS[q] or world.QUALITY_COLORS.normal
end

-- world.item_color(item | id) -> ARGB | nil: the colour the game gives the item's label (as quality_color, but class items
-- take their class's colour unless the game's UNIQUE_COLOR is on); nil when the item is gone.
function world.item_color(item)
    local id = type(item) == 'table' and item.id or item
    if type(id) ~= 'number' then return nil end
    return native.item_color(id)
end

-- Whether an item (world.item's table) can be used by class `name` ('darkelf' ...).
function world.usable_by(item, name)
    if #item.classes == 0 then return true end
    for _, c in ipairs(item.classes) do if c == name then return true end end
    return false
end

-- Items lying in the world within `radius` of (x, y) on opts.layer (default 0), nearest first: world.item tables.
function world.items_near(x, y, radius, opts)
    assert(type(x) == 'number' and type(y) == 'number' and type(radius) == 'number', 'x, y, radius must be numbers')
    opts = opts or {}
    local out = {}
    for _, id in ipairs(native.items_near(x, y, opts.layer or 0, radius, opts.limit or 64)) do
        local it = world.item(id)
        if it then out[#out + 1] = it end
    end
    return out
end

-- Inventories: gear.inv. The game keeps a hero's backpack, the chest and so on as grids of cells ("containers", 32 of
-- them: the backpack of the hero with object id n is container n, the chest is container 17).
local inv = {CHEST = 17}

local Container = {}
Container.__index = Container

local function make_container(index)
    local info = type(index) == 'number' and native.inv_container(index)
    if not info then return nil end
    return setmetatable({index = index, columns = info.columns, rows = info.rows}, Container)
end

-- inv.container(which) -> container | nil. `which`: 'chest', 'backpack' (the local hero's) or the number (0..31).
function inv.container(which)
    if which == 'chest' then which = inv.CHEST
    elseif which == 'backpack' then which = native.hero()
    end
    if type(which) ~= 'number' then return nil end
    return make_container(which)
end

-- container:items() -> the items in it, in cell order: world.item's table plus `col`, `row` (top-left cell, from 0),
-- `w`, `h` (size in cells).
function Container:items()
    local out = {}
    for _, e in ipairs(native.inv_items(self.index)) do
        local it = world.item(e.id)
        if it then
            it.col, it.row, it.w, it.h = e.x, e.y, e.w, e.h
            out[#out + 1] = it
        end
    end
    return out
end

-- container:free_spot(w, h) -> col, row | nil: room for a w x h item.
function Container:free_spot(w, h)
    return native.inv_find_space(self.index, w, h)
end

-- container:put(item_id, col, row) -> true | nil, message: puts an item that lies in the world or was taken out of a
-- container at that cell.
function Container:put(item_id, col, row)
    return native.inv_put(self.index, item_id, col, row)
end

-- container:take(item_id) -> true | nil, message: takes the item out; the item object stays (put it somewhere or
-- inv.destroy it).
function Container:take(item_id)
    return native.inv_remove(self.index, item_id)
end

-- inv.can_change() -> allowed, network: whether the local hero's inventory may be changed now (single player as
-- world.can_change; in a network game too, through the engine's announcing calls, for the hero's own backpack and the
-- chest), and whether a network game runs.
function inv.can_change()
    return native.inv_can_change()
end

-- inv.size(item_id) -> w, h | nil: the size in cells.
function inv.size(item_id)
    return native.inv_size(item_id)
end

-- inv.socket(item_id, upgrade_id) -> true | nil, message: puts the upgrade item (a rune) into a free socket of the
-- item, as the blacksmith does; the rune is consumed.
function inv.socket(item_id, upgrade_id)
    return native.inv_socket(item_id, upgrade_id)
end

-- inv.export(item_id) -> bytes | nil, message: the item's type and own data as a binary string (not where it is).
function inv.export(item_id)
    return native.inv_export(item_id)
end

-- inv.values(item_id_or_bytes) -> table | nil, message: the item's resolved value record decoded (its own 0x82-byte block of
-- sWeaponInfo: level, requirements, damage, attack / defense / protection, the eight bonus slots) plus the records its sockets
-- and random properties merged into it, from `export`'s bytes (or the item's id). Numbers as stored: what the item's tooltip
-- derives its lines from (a bonus scaled by a hero attribute shows its stored percentage times the hero's attribute).
do      -- (a block: the chunk's local variables are nearly used up)
local function u8(s, i) return s:byte(i + 1) or 0 end
local function u16(s, i) return u8(s, i) + u8(s, i + 1) * 256 end
local function i16(s, i) local v = u16(s, i) return v >= 32768 and v - 65536 or v end
local function u32(s, i) return u16(s, i) + u16(s, i + 2) * 65536 end
local function decode_record(s, at)
    local r = {
        type = u32(s, at), flags = u32(s, at + 4), price = u32(s, at + 8),
        level = u8(s, at + 25),
        requirements = {level = u8(s, at + 20), strength = u8(s, at + 21), dexterity = u8(s, at + 22), charisma = u8(s, at + 23),
            endurance = u8(s, at + 24), skill = u8(s, at + 26), skill_level = u8(s, at + 27)},
        attack = i16(s, at + 44), defense = i16(s, at + 46), bw = i16(s, at + 48),
        protection = {physical = i16(s, at + 50), fire = i16(s, at + 52), magic = i16(s, at + 54), poison = i16(s, at + 56)},
        damage = {min = {}, max = {}}, sockets = {}, bonuses = {},
        blacksmith_level = u8(s, at + 129),
    }
    for i = 0, 3 do
        r.damage.min[i + 1] = u16(s, at + 28 + i * 2)
        r.damage.max[i + 1] = i16(s, at + 36 + i * 2)
    end
    for i = 0, 7 do
        r.sockets[i + 1] = u8(s, at + 12 + i)
        local group, kind, value = u32(s, at + 74 + i * 4), u16(s, at + 58 + i * 2), i16(s, at + 106 + i * 2)
        if group ~= 0 or kind ~= 0 then
            r.bonuses[#r.bonuses + 1] = {slot = i, code = group % 65536, attribute = math.floor(group / 65536) % 256,
                flags = kind, value = value}
        end
    end
    return r
end
function inv.values(item)
    local bytes = item
    local id
    if type(item) == 'number' then id = item bytes = native.inv_export(item) end
    if type(bytes) ~= 'string' then return nil, 'no such item' end
    if #bytes < 6 + 130 then return nil, 'not an item export' end
    local n = u16(bytes, 4)
    if #bytes < 6 + (n + 1) * 130 then return nil, 'not an item export' end
    local v = decode_record(bytes, 6 + n * 130)
    v.id = id
    v.entries = {}
    -- A socket record i (1-based) counts when the item's socket kind i is 1..4 (the engine's rule for merging them).
    for i = 1, n do
        local e = decode_record(bytes, 6 + (i - 1) * 130)
        e.entry = i
        e.active = v.sockets[i] ~= nil and v.sockets[i] >= 1 and v.sockets[i] <= 4
        v.entries[i] = e
        for _, b in ipairs(e.bonuses) do b.entry = i end
    end
    for _, b in ipairs(v.bonuses) do b.entry = 0 end
    return v
end
end

-- inv.bonus_text(item_id, entry, slot) -> text | nil: the game's own line for a bonus: slot 0..7 of the item's record (entry 0) or of
-- one of its merged records (1..n), UTF-8 with the game's colour codes, as the tooltip shows it (the hero's attributes included).
function inv.bonus_text(item_id, entry, slot)
    return native.item_bonus_text(item_id, entry or 0, slot or 0)
end

-- inv.create(bytes) -> item_id | nil, message: a new item from export's bytes, lying at the hero's feet.
function inv.create(bytes)
    return native.inv_import(bytes)
end

-- inv.destroy(item_id) -> true | nil, message: deletes an item that is in no container.
function inv.destroy(item_id)
    return native.inv_destroy(item_id)
end

-- inv.add(container, bytes) -> item_id, col, row | nil, message: a new item from export's bytes straight into the
-- container (the first spot with room). Nothing changes when it fails.
function inv.add(container, bytes)
    local id, err = native.inv_import(bytes)
    if not id then return nil, err end
    local w, h = native.inv_size(id)
    local col, row
    if w then col, row = container:free_spot(w, h) end
    if not col then
        native.inv_destroy(id)
        return nil, 'no room'
    end
    local ok, perr = container:put(id, col, row)
    if not ok then
        native.inv_destroy(id)
        return nil, perr
    end
    return id, col, row
end

-- inv.remove(container, item_id) -> bytes | nil, message: takes the item out of the container, destroys it and
-- returns its export. If it fails the item is back where it was.
function inv.remove(container, item_id)
    local bytes, err = native.inv_export(item_id)
    if not bytes then return nil, err end
    local found
    for _, e in ipairs(native.inv_items(container.index)) do
        if e.id == item_id then found = e break end
    end
    if not found then return nil, 'the item is not in that container' end
    local ok, terr = native.inv_remove(container.index, item_id)
    if not ok then return nil, terr end
    local dok, derr = native.inv_destroy(item_id)
    if not dok then
        native.inv_put(container.index, item_id, found.x, found.y)
        return nil, derr
    end
    return bytes
end

-- inv.hex(bytes) -> text, inv.unhex(text) -> bytes | nil: for keeping the bytes in JSON.
function inv.hex(bytes)
    return (bytes:gsub('.', function(c) return string.format('%02x', c:byte()) end))
end

function inv.unhex(text)
    if type(text) ~= 'string' or #text % 2 ~= 0 or text:find('[^0-9a-fA-F]') then return nil end
    return (text:gsub('%x%x', function(h) return string.char(tonumber(h, 16)) end))
end

-- Pickup rules per mod (gear.world.pickup_rules): merged after each tick and handed to the native side.
local pickup_rules = {}     -- mod -> {block = {id = true}, dim = {id = true}, dim_alpha}
local pickup_dirty = false

local function publish_pickup_rules()
    if not pickup_dirty then return end
    pickup_dirty = false
    local blocked, dimmed, alpha = {}, {}, 255
    for mod, r in pairs(pickup_rules) do
        if not mod.disabled then
            for id in pairs(r.block) do blocked[#blocked + 1] = id end
            for id in pairs(r.dim) do dimmed[#dimmed + 1] = id end
            alpha = math.min(alpha, r.dim_alpha)
        end
    end
    native.pickup_rules(blocked, dimmed, alpha)
end

-- A random point between `min` and `max` world units from (x, y).
function world.around(x, y, min, max)
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    min, max = min or 0, max or min or 0
    local a = math.random() * 2 * math.pi
    local r = min + math.random() * (max - min)
    return math.floor(x + math.cos(a) * r + 0.5), math.floor(y + math.sin(a) * r + 0.5)
end

-- spawn(type, x, y, opts) -> creature | nil, message. `type` is a type id or an object type name;
-- opts.layer (default: the hero's), opts.label (a text kept for the creature, see creature:label()), opts.modifiers (see creature:set_modifiers).
local function spawn(mod, kind, x, y, opts)
    opts = opts or {}
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    local id = kind
    if type(kind) == 'string' then
        id = data.object_types:find(kind)
        if not id then return nil, 'no object type named ' .. kind end
    end
    assert(type(id) == 'number', 'type must be a type id or name')
    local layer = opts.layer
    if not layer then
        local h = native.hero()
        local o = h and native.object(h)
        layer = o and o.layer or 0
    end
    local oid, err = native.spawn(id, math.floor(x), math.floor(y), layer)
    if not oid then return nil, err end
    if opts.label ~= nil then
        labels[oid] = {text = tostring(opts.label), mod = mod.id}
    end
    if opts.modifiers ~= nil then
        local c = wrap(oid)
        local ok, err = c:set_modifiers(opts.modifiers)
        if not ok then
            c:remove()
            return nil, err
        end
    end
    log('[%s] spawned %s (type %d) as %d at %d,%d', mod.id, data.object_types:name(id) or '?', id, oid, x, y)
    return wrap(oid)
end

---------------------------------------------------------------------------------------------------------------
-- UI drawing (gear.ui). Immediate mode: what a mod draws during a tick (its 'frame' and 'draw' handlers, timers,
-- tasks) is shown until the next tick, then replaced; the native side draws it inside the game's UI pass. Positions
-- are the UI's 1024x768 layout. Colors are 0xAARRGGBB numbers (gear.ui.rgba).
---------------------------------------------------------------------------------------------------------------
local UI_LAYERS = {world = 0, hud = 1, top = 2}
local UI_FITS = {stretch = 0, contain = 1, fit = 1, cover = 2, fill = 2}
UI_FITS[1] = {h = 4, v = 8, hv = 12, vh = 12}          -- [1]: added to the fit for native.ui_image (a flip)
local UI_ALIGN = {left = 0, center = 1, right = 4}   -- the fonts' flags
local UI_VALIGN = {top = 0, middle = 1, bottom = 2, baseline = 3}
local UI_MAX_PER_MOD = 2000     -- draw commands per mod and tick
local ui_counts = {}            -- mod -> commands this tick
local ui_slots = nil            -- window name (upper case) -> slot, this tick

local function ui_color(c, default)
    if c == nil then return default end
    if type(c) == 'number' then return c % 4294967296 end
    if type(c) == 'table' then
        local function b(v, d) v = v or d; return math.max(0, math.min(255, math.floor(v + 0.5))) end
        return ((b(c[4] or c.a, 255) * 256 + b(c[1] or c.r, 0)) * 256 + b(c[2] or c.g, 0)) * 256 + b(c[3] or c.b, 0)
    end
    error('a color is 0xAARRGGBB or {r, g, b, a}', 3)
end

local function ui_window_slot(name)
    if not ui_slots then
        ui_slots = {}
        for _, w in ipairs(native.ui_windows()) do ui_slots[w.name:upper()] = w.slot end
    end
    return ui_slots[name:upper()]
end

-- The layer and window slot of opts; nil if the window does not exist (then nothing is drawn).
local function ui_target(opts)
    local layer = UI_LAYERS[opts.layer or 'hud']
    if not layer then error('layer is "world", "hud" or "top"', 3) end
    local slot = -1
    if opts.window ~= nil then
        if type(opts.window) ~= 'string' then error('window is a control name ("UI_WND_INVENTORY")', 3) end
        slot = ui_window_slot(opts.window)
        if not slot then return nil end
    end
    return layer, slot
end

local function ui_count(mod, count)
    local n = (ui_counts[mod] or 0) + (count or 1)
    if n > UI_MAX_PER_MOD then error('too many draw commands this frame (' .. UI_MAX_PER_MOD .. ')', 3) end
    ui_counts[mod] = n
end

-- Input for widgets. Regions (gear.ui.region and every widget) are rectangles that take the mouse: a press over the
-- topmost one is taken from the game and reported with the next tick. Hover is decided against the previous
-- frame's regions (the topmost under the cursor), so a dropdown's open list hides what lies below it.
local ui_handles = {}       -- key (mod id .. '\0' .. widget id) -> handle
local ui_handle_key = {}    -- handle -> key
local ui_next_handle = 1
local ui_regions = {}       -- this tick's regions, in order: {handle, layer, x, y, w, h}
local ui_prev_regions = {}
local ui_input = {}         -- handle -> {pressed, released, clicked} of this tick
local ui_active = nil       -- the handle whose press has no release yet
local ui_hot = nil          -- the topmost region under the cursor
local ui_mouse = {x = 0, y = 0}
local ui_tick_state = nil   -- native.ui_state() of this tick
local ui_tooltip = nil      -- {text, mod} to draw at the end of the tick
local ui_hover_since = {}   -- handle -> time the cursor came over it
local ui_dropdown = nil     -- the open dropdown's handle
local ui_keys = {}          -- mod -> vk -> held at the last query
local key_capture = {}      -- mod -> true: the mod keeps the keyboard from the game (ui.capture_keys)
local ui_panel_list = {}    -- the panels of all mods drawn this tick, in order: {key, title, x, y, w, h, ...}
local ui_panel_prev = {}    -- the last tick's
-- The controller (SacredBild's D-pad): the widgets' rectangles are reported as stops each tick; the pad's events
-- (slider nudges, B, the mod menu button) come back.
local ui_nav = {}           -- this tick's stops: {mod, x, y, w, h, layer, kind, ax, ay, drop}
local ui_nav_modal = nil    -- the mod whose screen takes the pad alone (ui.nav_modal)
local ui_window_open = false    -- a closable panel is up this tick
local ui_pad = {nudge = 0, back = 0, menu = 0}
local ui_menu_items = {}    -- mod -> {{id, label, fn}} (ui.menu_item)
-- Clipping and scrolling: a scroll area clips what is drawn inside it (and the regions: a widget scrolled out of
-- sight takes no mouse); things that must lie over everything (an open dropdown's list) are drawn after all draw
-- handlers (uis.defer).
local uis = {}            -- the scroll areas' and deferred drawing's state (one local: the main chunk has few left)
uis.clip_stack = {}    -- this tick's open clips {x, y, w, h}, innermost last
uis.deferred = {}      -- functions run after every mod's draw handlers, in order
uis.wheel = 0          -- wheel notches over mod UI this tick (+ away from the player, as WM_MOUSEWHEEL)
uis.wheel_taken = false
uis.pad_scroll = 0     -- D-pad lines in a scroll area this tick (+ down)
uis.scroll_now = {}    -- this tick's scroll areas {key, x, y, w, h, layer}
uis.scroll_prev = {}
uis.scroll_hot = nil   -- the key of the topmost scroll area under the cursor (last tick's)
uis.scroll_focus = nil -- the key of the scroll area clicked last (its keys work while the cursor is elsewhere)
uis.SCROLLBAR = 12     -- the scrollbar's width
-- Replaced windows (ui.replace_window): name -> the mod that draws its own screen in the window's place. Published
-- to Gear every tick before the handlers run; a mod that is off or unloaded does not count (the game's window shows).
uis.replace = {}
uis.typed = ''         -- the characters typed since the last tick (while a mod captures the keys)
function uis.publish_replace()
    local names = {}
    for name, m in pairs(uis.replace) do
        if m.disabled or m.unloaded then uis.replace[name] = nil else names[#names + 1] = name end
    end
    if native.ui_replace_claims then native.ui_replace_claims(names) end
end

function uis.defer(fn)
    uis.deferred[#uis.deferred + 1] = fn
end

function uis.set_clip(c)
    if not native.ui_clip then return end
    if c then native.ui_clip(c.x, c.y, c.w, c.h) else native.ui_clip() end
end

-- Scroll math (also for the kernel tests): the offset clamped to 0..max, the thumb's size and place in a track.
function uis.scroll_clamp(offset, content, view)
    local max = math.max(0, content - view)
    return math.max(0, math.min(max, offset)), max
end
function uis.scroll_thumb(offset, content, view, track)
    local max = math.max(0, content - view)
    local size = math.max(16, math.min(track, track * view / math.max(content, 1)))
    local pos = max > 0 and (track - size) * offset / max or 0
    return size, pos
end
function uis.scroll_from_thumb(pos, content, view, track, size)
    local max = math.max(0, content - view)
    if track - size <= 0 then return 0 end
    return math.max(0, math.min(max, pos / (track - size) * max))
end
kernel.scroll_math = {clamp = uis.scroll_clamp, thumb = uis.scroll_thumb, from_thumb = uis.scroll_from_thumb}

local function ui_handle(mod, id)
    local key = mod.id .. '\0' .. tostring(id)
    local h = ui_handles[key]
    if not h then
        h = ui_next_handle
        ui_next_handle = ui_next_handle + 1
        ui_handles[key] = h
        ui_handle_key[h] = key
    end
    return h
end

local function ui_begin_tick()
    ui_counts = {}
    ui_slots = nil
    ui_prev_regions, ui_regions = ui_regions, {}
    ui_panel_prev, ui_panel_list = ui_panel_list, {}
    ui_tooltip = nil
    ui_nav, ui_window_open = {}, false
    do
        local nudge, back, menu = native.ui_pad()
        ui_pad = {nudge = nudge or 0, back = back or 0, menu = menu or 0}
        -- B closes an open dropdown first.
        if ui_pad.back > 0 and ui_dropdown then
            ui_dropdown = nil
            ui_pad.back = ui_pad.back - 1
        end
    end
    uis.clip_stack, uis.deferred = {}, {}
    uis.set_clip(nil)
    do
        local wheel, pad = 0, 0
        if native.ui_wheel then wheel, pad = native.ui_wheel() end
        uis.wheel, uis.pad_scroll, uis.wheel_taken = (wheel or 0) / 120, pad or 0, false
    end
    uis.scroll_prev, uis.scroll_now = uis.scroll_now, {}
    uis.modal, uis.modal_next = uis.modal_next, nil      -- the document that was modal in the last frame (draw order between mods is not fixed)
    ui_tick_state = native.ui_state()
    ui_mouse.x, ui_mouse.y = ui_tick_state.mouse_x or 0, ui_tick_state.mouse_y or 0
    -- The topmost scroll area under the cursor takes the wheel (layers in draw order, then the order drawn).
    uis.scroll_hot = nil
    do
        local best = -1
        for i, a in ipairs(uis.scroll_prev) do
            local rank = a.layer * 100000 + i
            if rank > best and ui_mouse.x >= a.x and ui_mouse.y >= a.y and ui_mouse.x < a.x + a.w and ui_mouse.y < a.y + a.h then
                uis.scroll_hot, best = a.key, rank
            end
        end
    end
    -- The topmost region of the last frame under the cursor (layers in draw order, then the order registered).
    ui_hot = nil
    local best = -1
    for i, r in ipairs(ui_prev_regions) do
        local rank = r.layer * 100000 + i
        if rank > best and ui_mouse.x >= r.x and ui_mouse.y >= r.y and ui_mouse.x < r.x + r.w and ui_mouse.y < r.y + r.h then
            ui_hot, best = r.handle, rank
        end
    end
    local now = native.time()
    for h in pairs(ui_hover_since) do
        if h ~= ui_hot then ui_hover_since[h] = nil end
    end
    if ui_hot and not ui_hover_since[ui_hot] then ui_hover_since[ui_hot] = now end
    ui_input = {}
    for _, e in ipairs(native.ui_events()) do
        local s = ui_input[e.region] or {}
        ui_input[e.region] = s
        -- The button's own position: the cursor read at the tick may still be the one before it moved there.
        ui_mouse.x, ui_mouse.y = e.x, e.y
        if e.down then
            if e.button == 0 then
                s.pressed = true
                ui_active = e.region
            end
        else
            s.released = true
            if ui_active == e.region then
                ui_active = nil
                -- A click: released over the region it was pressed on.
                for _, r in ipairs(ui_prev_regions) do
                    if r.handle == e.region and e.x >= r.x and e.y >= r.y and e.x < r.x + r.w and e.y < r.y + r.h then
                        s.clicked = true
                    end
                end
            end
        end
    end
end

-- Fonts. nil or 'ui': the vector UI font (drawn for the screen's pixels; opts.size in layout units, opts.face, bold,
-- italic, shadow, outline); a number or 'game1'..'game7': the game's own bitmap font of that id (scaled with the UI).
local UI_TEXT_SIZE = 13
local function ui_font(opts, level)
    local f = opts.font
    if f == nil or f == 'ui' then return -1 end
    if type(f) == 'number' then return f end
    local n = type(f) == 'string' and f:match('^game(%d+)$')
    if n then return tonumber(n) end
    error('font is "ui", "game1".."game7" or a game font id', (level or 2) + 1)
end
local function ui_font_style(opts)
    return bit.bor(opts.bold and 1 or 0, opts.italic and 2 or 0, opts.shadow and 4 or 0, opts.outline and 8 or 0)
end
-- Width and height of text in layout units, or nil.
local function ui_measure(text, opts)
    local font = ui_font(opts, 2)
    if font == -1 then
        return native.ui_text_size(text, -1, opts.face, opts.size or UI_TEXT_SIZE, ui_font_style(opts))
    end
    return native.ui_text_size(text, font)
end

-- The tooltip of the widget hovered long enough, drawn last on the top layer beside the cursor.
local UI_TOOLTIP_DELAY = 0.4
-- The game's cursor sprite (an arrow whose tip is the hotspot) reaches this far right and down of the hotspot, in layout
-- units (measured at the 1024 x 768 layout; the sprite is smaller in layout units at higher resolutions).
local UI_CURSOR_W, UI_CURSOR_H = 30, 38

-- Where a w x h tooltip goes for a cursor at (mx, my): the first of below-right, below-left, above-right, above-left,
-- right of and left of the cursor that lies inside `bounds` ({left, top, right, bottom}) and clear of the cursor
-- sprite (cw x ch from the hotspot); if none does, the first clamped into the bounds and pushed off the sprite.
local function tooltip_place(w, h, mx, my, bounds, cw, ch, gap)
    local l, t, r, b = bounds[1], bounds[2], bounds[3], bounds[4]
    local function inside(x, y) return x >= l and y >= t and x + w <= r and y + h <= b end
    -- candidates: below / above the sprite, left or right aligned to the hotspot; beside it, level with the hotspot
    local cands = {
        {mx + gap, my + ch + gap},
        {mx - w - gap, my + ch + gap},
        {mx + gap, my - h - gap},
        {mx - w - gap, my - h - gap},
        {mx + cw + gap, my},
        {mx - w - gap, my},
    }
    for _, c in ipairs(cands) do
        if inside(c[1], c[2]) then return c[1], c[2] end
    end
    -- last resort: clamp the preferred place, then move it off the sprite (up, else right, else left)
    local x = math.max(l, math.min(cands[1][1], r - w))
    local y = math.max(t, math.min(cands[1][2], b - h))
    if x < mx + cw and x + w > mx and y < my + ch and y + h > my then
        if my - h - gap >= t then y = my - h - gap
        elseif mx + cw + gap + w <= r then x = mx + cw + gap
        else x = math.max(l, mx - w - gap) end
    end
    return x, y
end
-- Tells SacredBild which widgets the pad can point at. An open dropdown's list, or the screen of a mod that asked
-- (ui.nav_modal), is all there is; else every widget, only those of the top layer while the game's menu or a
-- message box is up.
local function ui_push_nav()
    local s = ui_tick_state or {}
    local list, flags = {}, 0
    local drop = {}
    for _, n in ipairs(ui_nav) do
        if n.drop then drop[#drop + 1] = n end
    end
    if #drop > 0 then
        list, flags = drop, 1
    elseif uis.modal then
        -- A modal RmlUi document: the pad stays on its stops (only they were registered), the game takes no pad input.
        for _, n in ipairs(ui_nav) do list[#list + 1] = n end
        flags = 1
    elseif ui_nav_modal then
        for _, n in ipairs(ui_nav) do
            if n.mod == ui_nav_modal then list[#list + 1] = n end
        end
        flags = 1
    else
        local top_only = s.escape_menu or s.message_box
        for _, n in ipairs(ui_nav) do
            if not top_only or n.layer == 2 then list[#list + 1] = n end
        end
        if ui_window_open then flags = 2 end
    end
    local out = {}
    for i, n in ipairs(list) do
        if i > 400 then break end
        out[i] = {x = n.ax or (n.x + n.w / 2), y = n.ay or (n.y + n.h / 2), left = n.x, top = n.y, width = n.w,
            height = n.h, kind = n.kind or 0}
    end
    native.ui_nav(out, flags)
end

-- Widest a tooltip's text gets before it wraps, in layout units.
uis.TOOLTIP_MAX_W = 360

-- text broken at spaces (and at \n) into lines no wider than `width`; a word wider than that keeps a line of its own.
-- Returns the lines and the width of the widest. (A field of uis: the kernel is at Lua's limit of 200 locals.)
function uis.tooltip_wrap(text, width)
    local lines, widest = {}, 0
    for para in (text .. '\n'):gmatch('(.-)\n') do
        local line, lw = nil, 0
        for word in para:gmatch('%S+') do
            local try = line and (line .. ' ' .. word) or word
            local tw = ui_measure(try, {}) or (#try * 6)
            if tw <= width or not line then
                line, lw = try, tw
            else
                lines[#lines + 1] = line
                widest = math.max(widest, lw)
                line = word
                lw = ui_measure(word, {}) or (#word * 6)
            end
        end
        lines[#lines + 1] = line or ''
        widest = math.max(widest, lw)
    end
    return lines, widest
end

local function ui_end_tick()
    -- What must lie over everything: open dropdown lists, drawn after all draw handlers, outside any clip.
    uis.clip_stack = {}
    uis.set_clip(nil)
    local deferred = uis.deferred
    uis.deferred = {}
    for _, fn in ipairs(deferred) do
        local ok, err = pcall(fn)
        if not ok then log('UI: a deferred draw failed: %s', tostring(err)) end
        uis.clip_stack = {}
        uis.set_clip(nil)
    end
    ui_push_nav()
    local t = ui_tooltip
    if not t then return end
    local s = ui_tick_state or {}
    local bl, bt = s.screen_left or 0, s.screen_top or 0
    local br, bb = s.screen_right or 1024, s.screen_bottom or 768
    -- wrap to at most uis.TOOLTIP_MAX_W units of text, and never wider than the screen less a margin
    local max_text = math.max(60, math.min(uis.TOOLTIP_MAX_W, (br - bl) - 24 - 12))
    local lines, tw = uis.tooltip_wrap(tostring(t.text), max_text)
    local _, line_h = ui_measure('Ag', {})
    line_h = math.max(line_h or 20, 14)
    local w = math.ceil(tw) + 12
    local h = #lines * line_h + math.max(0, 20 - line_h)
    local x, y = tooltip_place(w, h, ui_mouse.x, ui_mouse.y, {bl, bt, br, bb}, UI_CURSOR_W, UI_CURSOR_H, 4)
    native.ui_rect(2, -1, x, y, w, h, 0xE0101018)
    native.ui_rect(2, -1, x, y, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y + h - 1, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y, 1, h, 0xFFC8AA5A)
    native.ui_rect(2, -1, x + w - 1, y, 1, h, 0xFFC8AA5A)
    local ty = y + math.floor(math.max(0, 20 - line_h) / 2)
    for i, line in ipairs(lines) do
        if line ~= '' then
            native.ui_text(2, -1, line, x + 6, ty + (i - 1) * line_h, 0, line_h, 0xFFF0F0F0, -1, 0, 0, 0, nil,
                UI_TEXT_SIZE, 0, 1)
        end
    end
end

-- The layout (mods\gear_layout.json): where the player put the mods' panels ("mod_id:panel_id") and the game's
-- windows ("game:UI_WND_INVENTORY"). Stored in a virtual 4096 x 4096 screen (the whole screen, whatever its
-- resolution and aspect), so a layout keeps its places when the resolution changes.
local layout = nil          -- key -> {x, y, w, h} in virtual units
local layout_dirty = false
local ui_edit = false       -- the layout edit mode: panels show their ids and resize grips

local function layout_get()
    if not layout then
        local text = native.layout_read and native.layout_read()
        local v = text and json.decode(text)
        layout = type(v) == 'table' and v or {}
    end
    return layout
end

-- A panel's open / closed state (`open`) lives in its entry too and outlives moves, resizes and resets of the place.
local function layout_set(key, entry)
    local lay = layout_get()
    local old = lay[key]
    if old and old.open ~= nil then
        entry = entry or {}
        entry.open = old.open
    end
    lay[key] = entry
    layout_dirty = true
end

local function screen_rect()
    local s = ui_tick_state or {}
    local l, t = s.screen_left or 0, s.screen_top or 0
    local r, b = s.screen_right or 1024, s.screen_bottom or 768
    if r - l < 1 then r = l + 1024 end
    if b - t < 1 then b = t + 768 end
    return l, t, r, b
end

local function to_virtual(x, y)
    local l, t, r, b = screen_rect()
    return (x - l) / (r - l) * 4096, (y - t) / (b - t) * 4096
end

local function from_virtual(vx, vy)
    local l, t, r, b = screen_rect()
    return l + vx / 4096 * (r - l), t + vy / 4096 * (b - t)
end

local function size_to_virtual(w, h)
    local l, t, r, b = screen_rect()
    return w / (r - l) * 4096, h / (b - t) * 4096
end

local function size_from_virtual(vw, vh)
    local l, t, r, b = screen_rect()
    return vw / 4096 * (r - l), vh / 4096 * (b - t)
end

-- The game's windows the layout moved: placed every tick (cheap), so they follow resolution changes and new games.
local function apply_game_layout()
    local lay = layout_get()
    if not next(lay) or not native.ui_can_move_windows() then return end
    for _, w in ipairs(native.ui_windows()) do
        local e = w.movable and lay['game:' .. w.name]
        if e then
            local sx, sy = from_virtual(e.x, e.y)
            native.ui_move_window(w.slot, sx - w.x, sy - w.y, true)
        end
    end
end

local function save_layout()
    if layout_dirty and not ui_active then
        layout_dirty = false
        native.layout_write(json.encode(layout_get()))
    end
end

-- Key names for gear.ui.key_pressed.
local UI_VK = {
    escape = 0x1B, enter = 0x0D, space = 0x20, tab = 0x09, shift = 0x10, control = 0x11, alt = 0x12,
    left = 0x25, up = 0x26, right = 0x27, down = 0x28, insert = 0x2D, delete = 0x2E, home = 0x24, ['end'] = 0x23,
    pageup = 0x21, pagedown = 0x22,
    ctrl = 0x11, backspace = 0x08, pause = 0x13, capslock = 0x14,
    semicolon = 0xBA, equals = 0xBB, comma = 0xBC, minus = 0xBD, period = 0xBE, slash = 0xBF, backquote = 0xC0,
    lbracket = 0xDB, backslash = 0xDC, rbracket = 0xDD, quote = 0xDE,
    multiply = 0x6A, add = 0x6B, subtract = 0x6D, decimal = 0x6E, divide = 0x6F,
}
for i = 0, 9 do UI_VK['num' .. i] = 0x60 + i end
for i = 1, 24 do UI_VK['f' .. i] = 0x6F + i end
-- Mouse buttons 3 to 5 (middle, and the two side buttons), as polled keys: bindable like any key. Mice with more
-- buttons (MMO mice) send those as keys, usually F13 to F24 or digits, which the names above cover.
UI_VK.mouse3, UI_VK.mouse4, UI_VK.mouse5 = 0x04, 0x05, 0x06
for c = string.byte('a'), string.byte('z') do UI_VK[string.char(c)] = c - 32 end
for c = string.byte('0'), string.byte('9') do UI_VK[string.char(c)] = c end

local function make_ui(mod)
    local ui = {WIDTH = 1024, HEIGHT = 768}
    function ui.rgba(r, g, b, a)
        return ui_color({r, g, b, a == nil and 255 or a})
    end
    -- ui.rect(x, y, w, h, color, {layer, window})
    function ui.rect(x, y, w, h, color, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        ui_count(mod)
        return native.ui_rect(layer, slot, x, y, w, h, ui_color(color, 0xC0000000))
    end
    -- ui.frame(x, y, w, h, color, thickness, opts): an outline, `thickness` (default 1) inside the box.
    function ui.frame(x, y, w, h, color, thickness, opts)
        local t = thickness or 1
        ui.rect(x, y, w, t, color, opts)
        ui.rect(x, y + h - t, w, t, color, opts)
        ui.rect(x, y + t, t, h - 2 * t, color, opts)
        return ui.rect(x + w - t, y + t, t, h - 2 * t, color, opts)
    end
    -- ui.text(text, x, y, {color, font, size, face, bold, italic, shadow, outline, width, height, align, background,
    -- padding, markup, flags, layer, window})
    function ui.text(text, x, y, opts)
        opts = opts or {}
        text = tostring(text)
        local font = ui_font(opts)
        -- Backslashes start the game fonts' markup (\c colors, \g images ...); plain text keeps them out.
        if font ~= -1 and not opts.markup then text = text:gsub('\\', '/') end
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local align = UI_ALIGN[opts.align or 'left']
        if not align then error('align is "left", "center" or "right"', 2) end
        local valign = UI_VALIGN[opts.valign or 'top']
        if not valign then error('valign is "top", "middle", "bottom" or "baseline"', 2) end
        ui_count(mod)
        return native.ui_text(layer, slot, text, x, y, opts.width or 0, opts.height or 0,
            ui_color(opts.color, 0xFFFFFFFF), font, bit.bor(align, opts.flags or 0) % 4294967296,
            ui_color(opts.background, 0), opts.padding or 0, opts.face, opts.size or UI_TEXT_SIZE, ui_font_style(opts),
            valign)
    end
    -- ui.image(id, x, y, w, h, {fit = 'stretch' | 'contain' | 'cover', flip = 'h' | 'v' | 'hv', color, layer, window}); w, h default to the
    -- image's own size.
    function ui.image(id, x, y, w, h, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local fit = UI_FITS[opts.fit or 'stretch']
        if not fit then error('fit is "stretch", "contain" or "cover"', 2) end
        if opts.flip then
            local flip = UI_FITS[1][opts.flip]
            if not flip then error('flip is "h", "v" or "hv"', 2) end
            fit = fit + flip
        end
        ui_count(mod)
        local cr = opts.crop
        if cr then
            return native.ui_image(layer, slot, id, x, y, w or 0, h or 0, ui_color(opts.color, 0xFFFFFFFF), fit,
                cr[1] or 0, cr[2] or 0, cr[3] or 1, cr[4] or 1)
        end
        return native.ui_image(layer, slot, id, x, y, w or 0, h or 0, ui_color(opts.color, 0xFFFFFFFF), fit)
    end
    -- ui.item_icon(item_or_type, x, y, w, h, {layer, window}): what the inventory shows for an item (its own 3D model),
    -- as large as fits the box, centered in it. `item_or_type`: an item table (gear.world.item, gear.inv container
    -- items: its object is drawn), a type id or a type name (an item of that type is made for the frame).
    function ui.item_icon(item, x, y, w, h, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local id, kind = 0, 0
        if type(item) == 'table' then
            id, kind = item.id or 0, item.type or 0
        elseif type(item) == 'number' then
            kind = item
        elseif type(item) == 'string' then
            kind = data.object_types:find(item) or error('no item type named ' .. item, 2)
        else
            error('item_icon wants an item, a type id or a type name', 2)
        end
        assert(type(x) == 'number' and type(y) == 'number' and type(w) == 'number' and type(h) == 'number',
            'x, y, w and h must be numbers')
        ui_count(mod)
        return native.ui_item_icon(layer, slot, x, y, w, h, id, kind)
    end
    -- ui.item_tooltip(item, x, y) -> true | nil, message: the game's own tooltip popup for an item (framed, name in the
    -- rarity colour, price, protection, level, bonuses) with its top-left corner at (x, y) of the 1024x768 layout, kept on the
    -- screen. `item`: an item table (gear.world.item, container:items()), an item id, or an export string
    -- (gear.inv.export: a temporary item is made to read the text from). Call it every frame the tooltip should show.
    function ui.item_tooltip(item, x, y)
        assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
        if type(item) == 'table' then item = item.id end
        if type(item) ~= 'number' and type(item) ~= 'string' then error('item_tooltip wants an item, an id or an export', 2) end
        return native.item_tooltip(item, x, y)
    end
    -- ui.item_tooltip_text(item) -> text | nil, message: the words of the game's item tooltip (UTF-8, with the game's
    -- colour codes) instead of a popup; same arguments as item_tooltip.
    function ui.item_tooltip_text(item)
        if type(item) == 'table' then item = item.id end
        if type(item) ~= 'number' and type(item) ~= 'string' then error('item_tooltip_text wants an item, an id or an export', 2) end
        return native.item_tooltip_text(item)
    end
    -- ui.hovered_item() -> item, x, y, w, h | nil: the item whose tooltip the game shows right now (cursor or pad cursor resting on
    -- an item in the backpack, chest, shop, equipment ...) as a gear.world.item table, and the tooltip's VISIBLE outer rectangle (the
    -- frame's bars, not the corner ornaments) in layout units. The popup's own numbers are the game canvas's (its origin is the
    -- screen's left edge, the layout's is the 1024x768 box's: they differ by screen_left in a wider window; a window's popups are
    -- in the window's frame, which SacredBild reports: all converted here) and its size is
    -- that of the text area, 3 short of the bars on the left and top, 6 and 4 on the right and bottom.
    function ui.hovered_item()
        local id, x, y, w, h, _, canvas = native.item_hover()
        if not id then return nil end
        local item = world.item(id)
        if not item then return nil end
        if canvas then
            local sl, st = ui.screen()
            x, y = x + sl, y + st
        end
        return item, x - 3, y - 3, w + 9, h + 7
    end
    -- ui.tooltip_anchor(w, h, {gap, margin, side}) -> x, y, side | nil: where a panel of the bars' outer size w x h (see
    -- ui.native.frame) goes to sit flush against the right side of the game's item tooltip, the corner ornaments of both touching
    -- (`gap` more space, default 0); on the left side when it does not fit on the right (`side` 'right' or 'left' forces one);
    -- kept `margin` (default 2) inside the screen and its ornaments' overhang.
    function ui.tooltip_anchor(w, h, opts)
        assert(type(w) == 'number' and type(h) == 'number', 'w and h must be numbers')
        opts = opts or {}
        local _, rx, ry, rw, rh = ui.hovered_item()
        if not rx then return nil end
        local inset, over = ui.native.insets()
        local gap = over.right + over.left + (opts.gap or 0)
        local margin = opts.margin or 2
        local sl, st, sr, sb = ui.screen()
        local side = opts.side
        if side ~= 'left' and side ~= 'right' then
            side = (rx + rw + gap + w + over.right > sr - margin) and 'left' or 'right'
        end
        local x = side == 'right' and rx + rw + gap or rx - gap - w
        x = math.max(sl + margin + over.left, math.min(x, sr - margin - over.right - w))
        local y = math.max(st + margin + over.top, math.min(ry, sb - margin - over.bottom - h))
        return x, y, side
    end
    function ui.image_info(id)
        local info = type(id) == 'number' and native.ui_image_info(id)
        if info then info.id = id end
        return info or nil
    end
    -- ui.images(filter): the UI images whose sheet name contains `filter` (any case), {id, sheet, width, height}.
    function ui.images(filter)
        local out, f = {}, filter and filter:lower()
        for id = 1, native.ui_image_count() - 1 do
            local info = native.ui_image_info(id)
            if info and (not f or info.sheet:lower():find(f, 1, true)) then
                info.id = id
                out[#out + 1] = info
            end
        end
        return out
    end
    -- ui.texture_image(name): the id of a texture of the game's Texture.pak (by file name, any case) for ui.image; nil if there is none.
    function ui.texture_image(name)
        assert(type(name) == 'string', 'name must be a string')
        return native.texture_image(name)
    end
    function ui.fonts() return native.ui_font_count() end

    ---------------------------------------------------------------------------------------------------------
    -- gear.ui.native: pieces of the game's own look, built from its UI images (ui.images). Each piece is drawn with
    -- ui.image / ui.rect, so layers, windows, the 2000 commands per tick and the 1024x768 layout apply as there.
    -- A style is a table of image ids and sizes; `frame` draws one. Add styles (ring, bar ...) next to `tooltip`.
    ---------------------------------------------------------------------------------------------------------
    local native_ui = {}
    ui.native = native_ui
    -- tooltip: the frame of the game's item tooltip (TL = 54, TR = 55, the bottom corners are those flipped upside down;
    -- bar 52 at the sides, 14 / 21 on top and bottom; the fill is the game's 0x9F000000).
    local NATIVE_FRAMES = {
        tooltip = {
            fill = 0x9F000000,
            tl = {id = 54, w = 37, h = 19}, tr = {id = 55, w = 37, h = 17},
            left = {id = 52, w = 6}, right = {id = 52, w = 7},
            top = {id = 14, h = 6}, bottom = {id = 14, h = 6, flip = 'v'},
            overhang = {left = 11, top = 5, right = 11, bottom = 5},     -- the corner ornaments reach beyond the bars
            inset = {left = 6, top = 6, right = 6, bottom = 6},
        },
    }
    -- ui.native.insets(style) -> {left, top, right, bottom}: how far the frame's border reaches into the box (the text goes
    -- inside it) and `overhang`, how far the corner ornaments reach out of it.
    function native_ui.insets(style)
        local st = NATIVE_FRAMES[style or 'tooltip']
        if not st then error('unknown frame style', 2) end
        return copy(st.inset), copy(st.overhang)
    end
    -- ui.native.frame(x, y, w, h, {style = 'tooltip', layer, window, fill = color | false, color = tint}) -> drawn
    function native_ui.frame(x, y, w, h, opts)
        opts = opts or {}
        local st = NATIVE_FRAMES[opts.style or 'tooltip']
        if not st then error('unknown frame style (known: tooltip)', 2) end
        local o = {layer = opts.layer, window = opts.window, color = opts.color}
        local function img(p, ix, iy, iw, ih)
            if iw > 0 and ih > 0 then
                ui.image(p.id, ix, iy, iw, ih, {layer = o.layer, window = o.window, color = o.color, flip = p.flip})
            end
        end
        if opts.fill ~= false then
            ui.rect(x, y, w, h, opts.fill or st.fill, {layer = o.layer, window = o.window})
        end
        local oh = st.overhang
        local lw, rw, th, bh = st.left.w, st.right.w, st.top.h, st.bottom.h
        img(st.top, x + lw, y, w - lw - rw, th)
        img(st.bottom, x + lw, y + h - bh, w - lw - rw, bh)
        img(st.left, x, y + th, lw, h - th - bh)
        img(st.right, x + w - rw, y + th, rw, h - th - bh)
        img({id = st.tl.id}, x - oh.left, y - oh.top, st.tl.w, st.tl.h)
        img({id = st.tr.id}, x + w + oh.right - st.tr.w, y - oh.top, st.tr.w, st.tr.h)
        img({id = st.tl.id, flip = 'v'}, x - oh.left, y + h + oh.bottom - st.tl.h, st.tl.w, st.tl.h)
        img({id = st.tr.id, flip = 'v'}, x + w + oh.right - st.tr.w, y + h + oh.bottom - st.tr.h, st.tr.w, st.tr.h)
        return true
    end

    ---------------------------------------------------------------------------------------------------------
    -- Widgets (immediate mode): call them every frame with the current values; they draw themselves, take the
    -- mouse over their rectangles and return what the player did since the last frame. `id` names a widget
    -- within the mod (any string or number, unique per widget). opts.tooltip shows a text after the cursor rests
    -- on the widget; opts.layer as for drawing (default 'hud'); opts.enabled = false greys it out.
    ---------------------------------------------------------------------------------------------------------
    local C = {
        bg = 0xC8141420, bg_hover = 0xD8282838, bg_down = 0xE03C3C50, edge = 0xFFC8AA5A, edge_dim = 0xFF6E5E3A,
        text = 0xFFF0F0F0, text_dim = 0xFF8C8C8C, accent = 0xFFFFD780, title = 0xE0302418,
    }
    ui.COLORS = C

    -- ui.region(id, x, y, w, h, opts) -> {hover, pressed, released, clicked, held}: a rectangle that takes the
    -- mouse, for widgets of a mod's own.
    function ui.region(id, x, y, w, h, opts)
        opts = opts or {}
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        if uis.modal and uis.modal ~= opts._rml_doc then
            -- A modal RmlUi document is open: everything else takes no mouse and is no pad stop.
            return {hover = false, pressed = false, released = false, clicked = false, held = false, hidden = true}
        end
        local handle = ui_handle(mod, id)
        local c = uis.clip_stack[#uis.clip_stack]
        if c then
            -- Inside a scroll area: only the visible part takes the mouse; nothing if it is scrolled out of sight.
            local x0, y0 = math.max(x, c.x), math.max(y, c.y)
            local x1, y1 = math.min(x + w, c.x + c.w), math.min(y + h, c.y + c.h)
            if x1 <= x0 or y1 <= y0 then
                return {hover = false, pressed = false, released = false, clicked = false, held = false, hidden = true}
            end
            x, y, w, h = x0, y0, x1 - x0, y1 - y0
        end
        ui_count(mod)
        native.ui_region(layer, x, y, w, h, handle)
        ui_regions[#ui_regions + 1] = {handle = handle, layer = layer, x = x, y = y, w = w, h = h, id = tostring(id), mod = mod.id}
        if opts.nav ~= false and opts.enabled ~= false then
            ui_nav[#ui_nav + 1] = {mod = mod, x = x, y = y, w = w, h = h, layer = layer, kind = opts._nav_kind,
                ax = opts._nav_x, ay = opts._nav_y, drop = opts._nav_drop}
        end
        local s = ui_input[handle] or {}
        local enabled = opts.enabled ~= false
        local r = {
            hover = ui_hot == handle,
            pressed = enabled and s.pressed or false,
            released = s.released or false,
            clicked = enabled and s.clicked or false,
            held = enabled and ui_active == handle,
        }
        if r.hover and opts.tooltip and ui_hover_since[handle] and native.time() - ui_hover_since[handle] >= UI_TOOLTIP_DELAY then
            ui_tooltip = {text = tostring(opts.tooltip):gsub('\\', '/'), mod = mod}
        end
        return r
    end
    local region = ui.region

    ---------------------------------------------------------------------------------------------------------
    -- gear.ui.rml: documents in RmlUi's HTML/CSS dialect (.rml, .rcss), drawn through the overlay and reporting back to Lua.
    -- A document is loaded once, then drawn every frame from the mod's draw handler (`doc:draw()`), which also feeds
    -- it the mouse, the pad (every button, link and input is a pad stop) and the keyboard (text fields), and hands
    -- the events of the listeners registered with `doc:on` to their Lua functions. Files come from the mod's own folder.
    ---------------------------------------------------------------------------------------------------------
    local rml = {}
    ui.rml = rml
    local Doc = {}
    Doc.__index = Doc
    uis.rml_docs = uis.rml_docs or {}
    local RML_KEYS = {'backspace', 'delete', 'enter', 'tab', 'escape', 'left', 'right', 'up', 'down', 'home', 'end', 'pageup', 'pagedown'}

    -- The names of the callbacks of data-event-* attributes (opts.calls: name -> function).
    function uis.rml_call_names(opts)
        local names = {}
        for name in pairs(opts.calls or {}) do names[#names + 1] = name end
        table.sort(names)
        return names
    end
    -- The theme mod's folder and stylesheet (a running mod whose mod.lua has `theme = 'file.rcss'`), for the natives; nothing with opts.theme = false.
    function uis.rml_theme(opts)
        if opts.theme == false or not uis.theme_provider then return end
        return uis.theme_provider()
    end
    -- doc.model: the data model's values, read and written like a table; writing a value (at any depth) has the bindings of the
    -- document refresh. The top-level keys are the ones the model was loaded with.
    function uis.rml_proxy(doc, raw, top)
        return setmetatable({}, {
            __index = function(_, k)
                local v = raw[k]
                if type(v) == 'table' then return uis.rml_proxy(doc, v, top or k) end
                return v
            end,
            __newindex = function(_, k, v)
                raw[k] = v
                local key = top or k
                if not doc.closed then native.rml_model_set(doc.id, key, doc.data[key]) end
            end,
        })
    end
    local function rml_open(id, opts)
        local doc = setmetatable({id = id, handlers = {}, opts = opts, calls = opts.calls or {}}, Doc)
        if opts.model then
            doc.data = opts.model
            doc.model = uis.rml_proxy(doc, doc.data)
        end
        uis.rml_docs[id] = {mod = mod, doc = doc}
        return doc
    end

    -- rml.load(path, {dp}) -> document | nil, message: path is a .rml file in the mod's folder. With dp = true the document is
    -- laid out in screen pixels with 1dp = one layout unit (text is rasterized for the screen: crisp); otherwise 1px = one layout unit.
    function rml.load(path, opts)
        assert(type(path) == 'string', 'path must be a string')
        if not native.rml_load then error('RmlUi is not available in this build', 2) end
        opts = opts or {}
        local id, err = native.rml_load(mod.dir, path, opts.dp and true or false, opts.model, uis.rml_call_names(opts), uis.rml_theme(opts))
        if not id then return nil, err end
        return rml_open(id, opts)
    end

    -- rml.load_string(markup, {dp}) -> document | nil, message: the same from a string of RML (relative links resolve in the mod's folder).
    function rml.load_string(markup, opts)
        assert(type(markup) == 'string', 'markup must be a string')
        if not native.rml_load_string then error('RmlUi is not available in this build', 2) end
        opts = opts or {}
        local id, err = native.rml_load_string(mod.dir, markup, opts.dp and true or false, opts.model, uis.rml_call_names(opts), uis.rml_theme(opts))
        if not id then return nil, err end
        return rml_open(id, opts)
    end

    -- rml.load_font(path, fallback) -> ok: a TTF / OTF file of the mod, for font-family in its documents (the default face is Segoe UI).
    function rml.load_font(path, fallback)
        assert(type(path) == 'string', 'path must be a string')
        return native.rml_font and native.rml_font(mod.dir, path, fallback and true or false) or false
    end

    local function rml_escape(text)
        return (tostring(text):gsub('&', '&amp;'):gsub('<', '&lt;'):gsub('>', '&gt;'))
    end

    -- doc:on(selector, event, fn) -> ok: fn(event, doc) runs when `event` ('click', 'change', 'submit', 'keydown' ...) reaches an
    -- element matching `selector` ('#id', '.class', 'button' ...; '' is the document), also one added later. event = {type, target (id of the
    -- element hit), current (id of the element listening), params = {name = text}}; a change of a form control carries params.value.
    function Doc:on(selector, event, fn)
        assert(type(fn) == 'function', 'on(selector, event, fn)')
        local listener = native.rml_listen(self.id, tostring(selector), tostring(event))
        if not listener then return false end
        self.handlers[listener] = fn
        return true
    end

    -- Reading and changing the document: `selector` finds the first matching element (nil answers: no such element).
    function Doc:html(selector, markup) return native.rml_html(self.id, selector, markup) end
    function Doc:text(selector, text)
        if text ~= nil then return native.rml_html(self.id, selector, rml_escape(text)) end
        return native.rml_html(self.id, selector)
    end
    -- doc:attr(selector, name [, value]): value true sets a flag attribute (disabled, checked), false removes the attribute.
    function Doc:attr(selector, name, value)
        if type(value) == 'number' then value = tostring(value) end
        return native.rml_attr(self.id, selector, name, value)
    end
    function Doc:class(selector, name, on) return native.rml_class(self.id, selector, name, on and true or false) end
    -- doc:style(selector, property, value): an inline style; nil or false takes it off again.
    function Doc:style(selector, property, value)
        if type(value) == 'number' then value = tostring(value) end
        return native.rml_style(self.id, selector, property, value or false)
    end
    -- doc:value(selector [, text]): the value of a form control (an input, a select, a textarea).
    function Doc:value(selector, text)
        if type(text) == 'number' then text = tostring(text) end
        return native.rml_value(self.id, selector, text)
    end
    -- Elements: doc:query(selector) -> element | nil, doc:query_all(selector) -> {element}. An element is a handle (it is addressed by a
    -- number the host gives it): removing it from the document makes its methods answer nothing, nothing crashes.
    local Element = {}
    Element.__index = Element
    function uis.rml_element(doc, handle)
        return setmetatable({doc = doc, handle = handle, sel = '[data-gh="' .. handle .. '"]'}, Element)
    end
    function Doc:query(selector)
        local h = native.rml_query(self.id, tostring(selector))
        return h and uis.rml_element(self, h) or nil
    end
    function Doc:query_all(selector)
        local out = {}
        for i, h in ipairs(native.rml_query(self.id, tostring(selector), true)) do out[i] = uis.rml_element(self, h) end
        return out
    end
    function Element:exists() return native.rml_tag(self.doc.id, self.sel) ~= nil end
    function Element:tag() return native.rml_tag(self.doc.id, self.sel) end
    function Element:query(selector)
        local h = native.rml_query(self.doc.id, self.sel .. ' ' .. tostring(selector))
        return h and uis.rml_element(self.doc, h) or nil
    end
    function Element:query_all(selector)
        local out = {}
        for i, h in ipairs(native.rml_query(self.doc.id, self.sel .. ' ' .. tostring(selector), true)) do out[i] = uis.rml_element(self.doc, h) end
        return out
    end
    function Element:parent()
        local h = native.rml_relatives(self.doc.id, self.sel, 'parent')
        return h and uis.rml_element(self.doc, h) or nil
    end
    function Element:children()
        local out = {}
        for i, h in ipairs(native.rml_relatives(self.doc.id, self.sel, 'children')) do out[i] = uis.rml_element(self.doc, h) end
        return out
    end
    -- element:append(tag [, {attribute = text}]) -> the new element (the last child), nil if there is no such parent.
    function Element:append(tag, attrs)
        local a
        if attrs then
            a = {}
            for k, v in pairs(attrs) do a[tostring(k)] = tostring(v) end
        end
        local h = native.rml_append(self.doc.id, self.sel, tostring(tag), a)
        return h and h ~= 0 and uis.rml_element(self.doc, h) or nil
    end
    function Element:remove() return native.rml_remove(self.doc.id, self.sel) end
    function Element:html(markup) return self.doc:html(self.sel, markup) end
    function Element:text(text) return self.doc:text(self.sel, text) end
    function Element:attr(name, value) return self.doc:attr(self.sel, name, value) end
    function Element:class(name, on) return self.doc:class(self.sel, name, on) end
    function Element:style(property, value) return self.doc:style(self.sel, property, value) end
    function Element:value(text) return self.doc:value(self.sel, text) end
    function Element:rect() return native.rml_rect(self.doc.id, self.sel) end
    -- element:on(event, fn): fn(event, element) when the event happens on the element (also for elements added later).
    function Element:on(event, fn)
        assert(type(fn) == 'function', 'on(event, fn)')
        return self.doc:on(self.sel, event, function(e) return fn(e, self) end)
    end

    -- doc:get(key) -> the plain value of a top-level key of the data model (a table for lists and records).
    function Doc:get(key) return self.data and self.data[key] end
    function Doc:show() native.rml_show(self.id, true) end
    function Doc:hide() native.rml_show(self.id, false) end

    function Doc:close()
        if self.closed then return end
        self.closed = true
        uis.rml_docs[self.id] = nil
        native.rml_close(self.id)
        if self.capturing then ui.capture_keys(false) end
    end

    -- doc:draw({layer}) -> commands drawn: call it every frame from a draw handler. It lets the document take the mouse where it has
    -- elements, makes every button, link and input a pad stop, reads the keys for a focused text field, runs the document's
    -- animations, calls the handlers of events that happened and draws it.
    function Doc:draw(opts)
        if self.closed then return 0 end
        opts = opts or {}
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        local id = self.id
        local mx, my = ui.mouse()
        if not self.primed then
            -- Lay the document out once, so its windows have their places before the first frame is drawn.
            self.primed = true
            native.rml_update(id, mx, my, false)
        end
        do
            -- The classes of <body> that tell the stylesheet whether and which button prompts to show: "pad" while the
            -- controller was used last, "pad-ps" or "pad-switch" for the style of the pad.
            local on, style = false, 0
            if native.ui_pad_info then on, style = native.ui_pad_info() end
            local key = (on and 'pad' or '') .. ':' .. tostring(style)
            if self.pad_key ~= key then
                self.pad_key = key
                native.rml_class(id, 'body', 'pad', on)
                native.rml_class(id, 'body', 'pad-ps', style == 1)
                native.rml_class(id, 'body', 'pad-switch', style == 2)
            end
        end
        local hover = false
        local ropts = {layer = opts.layer, nav = false, _rml_doc = id}
        if opts.modal and not ui_edit then
            -- A modal dialog: it blocks the mouse and the pad for the game and for every other overlay until it is closed.
            uis.modal_next = id
            local sl, st, sr, sb = screen_rect()
            region('rml:' .. id .. ':modal', sl, st, sr - sl, sb - st, ropts)
            hover = true
        end
        local panels = self.panels or {}
        self.panels = panels
        for i, r in ipairs(native.rml_rects(id)) do
            if r.panel and r.panel ~= '' then
                -- A window the player may move (data-panel="name"): a panel of the layout like the ones of gear.ui.panel, so the layout
                -- editor lists it and keeps its place (mods\gear_layout.json, key "<mod id>:<name>").
                local key = mod.id .. ':' .. r.panel
                local p = panels[r.panel]
                if not p then p = {x = r.x, y = r.y}; panels[r.panel] = p end      -- where the document puts it
                local x, y = p.x, p.y
                local saved = layout_get()[key]
                if saved and saved.x then x, y = from_virtual(saved.x, saved.y) end
                local sl, st, sr, sb = screen_rect()
                x, y = math.max(sl, math.min(x, sr - r.w)), math.max(st, math.min(y, sb - r.h))
                if math.abs(x - r.x) > 0.25 or math.abs(y - r.y) > 0.25 then
                    if native.rml_move(id, '[data-panel="' .. r.panel .. '"]', x, y) then r.x, r.y = x, y end
                end
                ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = r.panel, title = r.title ~= '' and r.title or r.panel,
                    x = r.x, y = r.y, w = r.w, h = r.h, closable = false, back = false, resizable = false, min_width = 0, min_height = 0,
                    layer = opts.layer or 'hud'}
                if ui_edit then ui.frame(r.x, r.y, r.w, r.h, C.accent, 1, {layer = opts.layer}) end
            end
            -- In the layout editor the windows take no mouse: the editor drags them.
            if not ui_edit and region('rml:' .. id .. ':w' .. i, r.x, r.y, r.w, r.h, ropts).hover then hover = true end
        end
        if not ui_edit then
            for i, r in ipairs(native.rml_nav(id)) do
                local res = region('rml:' .. id .. ':n' .. i, r.x, r.y, r.w, r.h, {layer = opts.layer, _rml_doc = id})
                if res.hover then hover = true end
                if res.clicked then native.rml_click(id, i) end
            end
        end
        if native.rml_wants_text(id) then
            ui.capture_keys(true)
            self.capturing = true
            local typed = ui.typed()
            if typed ~= '' then native.rml_text(id, typed) end
            for _, name in ipairs(RML_KEYS) do
                if ui.key_pressed(name) then
                    native.rml_key(id, UI_VK[name], true)
                    native.rml_key(id, UI_VK[name], false)
                end
            end
        elseif self.capturing then
            ui.capture_keys(false)
            self.capturing = false
        end
        if hover and uis.wheel ~= 0 and not uis.wheel_taken then
            uis.wheel_taken = true
            native.rml_wheel(id, uis.wheel)
        end
        for _, e in ipairs(native.rml_update(id, mx, my, hover)) do
            local fn = e.call and self.calls[e.call] or self.handlers[e.listener]
            if e.call and fn then
                local ok, err = xpcall(fn, traceback, e.args, self, e)
                if not ok then log('[%s] rml callback %s failed: %s', mod.id, e.call, tostring(err)) end
                fn = nil
            end
            if fn then
                local ok, err = xpcall(fn, traceback, e, self)
                if not ok then log('[%s] rml handler for %s failed: %s', mod.id, e.type, tostring(err)) end
            end
            if self.closed then return 0 end
        end
        if self.data then
            -- A value the document changed itself (data-value): into the table, and the mod's on_change.
            local changed = native.rml_model_changed(id)
            if changed then
                for k, v in pairs(changed) do self.data[k] = v end
                if self.opts.on_change then
                    local ok, err = xpcall(self.opts.on_change, traceback, self, self.data)
                    if not ok then log('[%s] rml on_change failed: %s', mod.id, tostring(err)) end
                end
            end
        end
        local n = native.rml_draw(id, layer, -1)
        if n > 0 then ui_count(mod, n) end
        return n
    end

    -- The cursor in layout coordinates, and whether the left button is held over a mod's widget.
    function ui.mouse() return ui_mouse.x, ui_mouse.y end
    -- ui.over_ui(): whether the cursor is over a region of a mod's UI (as of the last frame).
    function ui.over_ui() return ui_hot ~= nil end

    -- ui.key_pressed(name): true once per press of the key ('f9', 'a', 'escape' ...), while the game has the focus.
    function ui.key_pressed(name)
        local vk = type(name) == 'number' and name or UI_VK[tostring(name):lower()]
        if not vk then error('unknown key ' .. tostring(name), 2) end
        local keys = ui_keys[mod] or {}
        ui_keys[mod] = keys
        local down = native.key_down(vk)
        local was = keys[vk]
        keys[vk] = down
        return down and not was
    end

    local scrolls = {}      -- scroll area id -> {offset, content, ...} (ui.scroll_begin), for the session
    local function box(x, y, w, h, fill, edge, opts)
        ui.rect(x, y, w, h, fill, opts)
        ui.frame(x, y, w, h, edge, 1, opts)
    end
    local function label_color(opts) return opts.enabled == false and C.text_dim or (opts.color or C.text) end

    -- ui.label(text, x, y, opts): text with the widgets' look (the UI font unless opts.font), a tooltip if opts.tooltip.
    function ui.label(id, text, x, y, opts)
        opts = opts or {}
        local w, h = ui_measure(tostring(text), opts)
        if opts.tooltip then region(id, x, y, w or 60, h or 14, {tooltip = opts.tooltip, layer = opts.layer, nav = false}) end
        return ui.text(text, x, y, {font = opts.font, size = opts.size, face = opts.face, bold = opts.bold,
            italic = opts.italic, height = opts.height, valign = opts.valign, color = label_color(opts),
            layer = opts.layer})
    end

    -- ui.button(id, x, y, w, h, text, opts) -> clicked
    function ui.button(id, x, y, w, h, text, opts)
        opts = opts or {}
        local r = region(id, x, y, w, h, opts)
        local fill = (r.held and C.bg_down) or (r.hover and C.bg_hover) or C.bg
        box(x, y, w, h, fill, r.hover and C.edge or C.edge_dim, opts)
        if opts.image then
            ui.image(opts.image, x + 2, y + 2, h - 4, h - 4, {fit = 'contain', layer = opts.layer})
        end
        ui.text(text, x, y, {width = w, height = h, align = 'center', valign = 'middle',
            color = label_color(opts), layer = opts.layer})
        return r.clicked
    end

    -- ui.checkbox(id, x, y, text, checked, opts) -> changed, checked
    function ui.checkbox(id, x, y, text, checked, opts)
        opts = opts or {}
        local tw = ui_measure(tostring(text), {}) or 60
        local r = region(id, x, y, 18 + tw, 16, opts)
        box(x, y + 1, 14, 14, r.hover and C.bg_hover or C.bg, r.hover and C.edge or C.edge_dim, opts)
        if checked then ui.rect(x + 3, y + 4, 8, 8, opts.enabled == false and C.text_dim or C.accent, opts) end
        ui.text(text, x + 20, y, {height = 16, valign = 'middle', color = label_color(opts), layer = opts.layer})
        if r.clicked then return true, not checked end
        return false, checked
    end

    -- A round mark of radius 6 from rectangles (the overlay has no circles).
    local function disc(cx, cy, rad, color, opts)
        for dy = -rad, rad - 1 do
            local yy = dy + 0.5
            local half = math.floor(math.sqrt(math.max(0, rad * rad - yy * yy)) + 0.5)
            ui.rect(cx - half, cy + dy, half * 2, 1, color, opts)
        end
    end

    -- ui.radio(id, x, y, text, selected, opts) -> clicked
    function ui.radio(id, x, y, text, selected, opts)
        opts = opts or {}
        local tw = ui_measure(tostring(text), {}) or 60
        local r = region(id, x, y, 18 + tw, 16, opts)
        disc(x + 7, y + 8, 7, r.hover and C.edge or C.edge_dim, opts)
        disc(x + 7, y + 8, 6, r.hover and C.bg_hover or 0xF0141420, opts)
        if selected then disc(x + 7, y + 8, 3, opts.enabled == false and C.text_dim or C.accent, opts) end
        ui.text(text, x + 20, y, {height = 16, valign = 'middle', color = label_color(opts), layer = opts.layer})
        return r.clicked
    end

    -- ui.radio_group(id, x, y, options, index, opts{horizontal, spacing}) -> changed, index
    function ui.radio_group(id, x, y, options, index, opts)
        opts = opts or {}
        local changed = false
        local px, py = x, y
        for i, text in ipairs(options) do
            local o = {tooltip = opts.tooltips and opts.tooltips[i], layer = opts.layer, enabled = opts.enabled}
            if ui.radio(tostring(id) .. '#' .. i, px, py, text, i == index, o) and i ~= index then
                index, changed = i, true
            end
            if opts.horizontal then
                px = px + 26 + (ui_measure(tostring(text), {}) or 60) + (opts.spacing or 8)
            else
                py = py + (opts.spacing or 18)
            end
        end
        return changed, index
    end


    -- Scroll areas. ui.scroll_begin(id, x, y, w, h, opts{step, layer, nav}) -> area {x, y, w, h, offset, max,
    -- content_x, content_y}; draw the content at content_y + its own y (what lies outside the viewport is clipped and
    -- takes no mouse), then ui.scroll_end(content_height) -> offset, max. The scrollbar (arrows, track, thumb) shows
    -- when the content is taller than the area; the wheel scrolls the area under the cursor, the keys (Up, Down,
    -- PageUp, PageDown, Home, End) the one under the cursor or clicked last, the D-pad (SacredBild) the one the
    -- pad's cursor is in. The offset is kept per id for the session; ui.scroll_to(id, offset) sets it.
    local scroll_stack = {}
    local function scroll_key_pressed(st, name)
        local vk = UI_VK[name]
        local down = native.key_down(vk)
        st.keys = st.keys or {}
        local was = st.keys[name]
        st.keys[name] = down
        return down and not was
    end
    function ui.scroll_begin(id, x, y, w, h, opts)
        opts = opts or {}
        local key = mod.id .. '\0' .. tostring(id)
        local st = scrolls[id] or {offset = 0, content = 0}
        scrolls[id] = st
        local layer = UI_LAYERS[opts.layer or 'hud']
        if not layer then error('layer is "world", "hud" or "top"', 2) end
        local step = opts.step or 20
        local bar = st.content > h and uis.SCROLLBAR or 0
        -- The body takes clicks on empty space (they focus the area) and is the pad's stop for it.
        local body = region(tostring(id) .. '#body', x, y, w, h, {layer = opts.layer, nav = opts.nav, _nav_kind = 2})
        if body.pressed then uis.scroll_focus = key end
        local delta = 0
        local hot = uis.scroll_hot == key
        if hot and uis.wheel ~= 0 and not uis.wheel_taken then
            delta = delta - uis.wheel * step * 3
            uis.wheel_taken = true
        end
        if (hot or uis.scroll_focus == key) and uis.pad_scroll ~= 0 then
            delta = delta + uis.pad_scroll * step
            uis.pad_scroll = 0
        end
        if hot or uis.scroll_focus == key then
            if scroll_key_pressed(st, 'down') then delta = delta + step end
            if scroll_key_pressed(st, 'up') then delta = delta - step end
            if scroll_key_pressed(st, 'pagedown') then delta = delta + h - step end
            if scroll_key_pressed(st, 'pageup') then delta = delta - h + step end
            if scroll_key_pressed(st, 'home') then delta = -math.huge end
            if scroll_key_pressed(st, 'end') then delta = math.huge end
        end
        local max
        st.offset, max = uis.scroll_clamp(st.offset + delta, st.content, h)
        uis.scroll_now[#uis.scroll_now + 1] = {key = key, x = x, y = y, w = w, h = h, layer = layer}
        local view = {x = x, y = y, w = w - bar, h = h}
        local outer = uis.clip_stack[#uis.clip_stack]
        local c = view
        if outer then
            local x0, y0 = math.max(view.x, outer.x), math.max(view.y, outer.y)
            local x1, y1 = math.min(view.x + view.w, outer.x + outer.w), math.min(view.y + view.h, outer.y + outer.h)
            c = {x = x0, y = y0, w = math.max(0, x1 - x0), h = math.max(0, y1 - y0)}
        end
        uis.clip_stack[#uis.clip_stack + 1] = c
        uis.set_clip(c)
        st.view, st.step, st.layer_name, st.id, st.full_w = view, step, opts.layer, id, w
        scroll_stack[#scroll_stack + 1] = st
        return {x = x, y = y, w = view.w, h = h, offset = st.offset, max = max, content_x = x, content_y = y - st.offset}
    end
    function ui.scroll_end(content_height)
        local st = table.remove(scroll_stack)
        if not st then error('scroll_end without scroll_begin', 2) end
        table.remove(uis.clip_stack)
        uis.set_clip(uis.clip_stack[#uis.clip_stack])
        if content_height then st.content = math.max(0, content_height) end
        local v = st.view
        local max
        st.offset, max = uis.scroll_clamp(st.offset, st.content, v.h)
        if max <= 0 then return st.offset, max end
        local o = {layer = st.layer_name}
        local id = tostring(st.id)
        local bx, bw = v.x + st.full_w - uis.SCROLLBAR + 1, uis.SCROLLBAR - 1
        local track_y, track_h = v.y + bw, v.h - 2 * bw
        local up = region(id .. '#up', bx, v.y, bw, bw, o)
        local down = region(id .. '#down', bx, v.y + v.h - bw, bw, bw, o)
        local track = region(id .. '#track', bx, track_y, bw, track_h, {layer = st.layer_name, nav = false})
        local size, pos = uis.scroll_thumb(st.offset, st.content, v.h, track_h)
        local thumb = region(id .. '#thumb', bx, track_y + pos, bw, size, {layer = st.layer_name, nav = false})
        local now = native.time()
        local function repeating(r)
            if r.pressed then st.repeat_at = now + 0.4; return true end
            if r.held and st.repeat_at and now >= st.repeat_at then st.repeat_at = now + 0.06; return true end
            return false
        end
        local offset = st.offset
        if repeating(up) then offset = offset - st.step end
        if repeating(down) then offset = offset + st.step end
        if thumb.pressed then st.grab = ui_mouse.y - (track_y + pos) end
        if thumb.held and st.grab then
            offset = uis.scroll_from_thumb(ui_mouse.y - st.grab - track_y, st.content, v.h, track_h, size)
        elseif not thumb.held then
            st.grab = nil
        end
        if track.clicked then offset = offset + (ui_mouse.y < track_y + pos and -v.h or v.h) end
        st.offset = uis.scroll_clamp(offset, st.content, v.h)
        size, pos = uis.scroll_thumb(st.offset, st.content, v.h, track_h)
        ui.rect(bx, v.y, bw, v.h, 0xC00C0C14, o)
        box(bx, track_y + pos, bw, size, (thumb.hover or thumb.held) and C.bg_down or C.bg_hover, C.edge, o)
        for _, a in ipairs({{up, v.y, -1}, {down, v.y + v.h - bw, 1}}) do
            local r, ay, dir = a[1], a[2], a[3]
            box(bx, ay, bw, bw, (r.hover or r.held) and C.bg_hover or C.bg, C.edge_dim, o)
            for i = 0, 2 do
                local row = dir < 0 and (2 - i) or i
                ui.rect(bx + 2 + i, ay + 4 + row, bw - 4 - 2 * i, 1, C.accent, o)
            end
        end
        return st.offset, max
    end
    function ui.scroll_to(id, offset)
        local st = scrolls[id] or {offset = 0, content = 0}
        scrolls[id] = st
        st.offset = math.max(0, offset or 0)
    end
    function ui.scroll_offset(id)
        local st = scrolls[id]
        return st and st.offset or 0, st and math.max(0, st.content - (st.view and st.view.h or 0)) or 0
    end

    -- ui.wheel(): wheel notches over mod UI this tick (+ away from the player, as Windows has it; 0 if none), and
    -- whether a scroll area already took them.
    function ui.wheel() return uis.wheel, uis.wheel_taken end

    -- ui.text_wrap(text, width, opts) -> lines, height: the text broken into lines that fit `width` (at spaces; a
    -- word longer than the width gets a line of its own; \n breaks too), and their height, in the font of `opts`
    -- (ui.text's). Cached per text, width and font.
    local wrap_cache = setmetatable({}, {__mode = 'k'})
    local wrap_count = 0
    function ui.text_wrap(text, width, opts)
        opts = opts or {}
        text = tostring(text)
        local fkey = table.concat({tostring(opts.font), tostring(opts.size), tostring(opts.face), tostring(opts.bold),
            tostring(opts.italic)}, '|')
        local key = text .. '\0' .. width .. '\0' .. fkey
        local hit = wrap_cache[key]
        if hit then return hit.lines, hit.height end
        local _, line_h = ui_measure('Ag', opts)
        line_h = line_h or 16
        local lines = {}
        for para in (text .. '\n'):gmatch('(.-)\n') do
            local line = nil
            for word in para:gmatch('%S+') do
                local try = line and (line .. ' ' .. word) or word
                local tw = ui_measure(try, opts) or (#try * 7)
                if tw <= width or not line then
                    line = try
                else
                    lines[#lines + 1] = line
                    line = word
                end
            end
            lines[#lines + 1] = line or ''
        end
        local height = #lines * line_h
        wrap_count = wrap_count + 1
        if wrap_count > 2000 then wrap_cache, wrap_count = setmetatable({}, {__mode = 'k'}), 0 end
        wrap_cache[key] = {lines = lines, height = height}
        return lines, height
    end

    -- ui.dropdown(id, x, y, w, options, index, opts{max_rows}) -> changed, index. The open list is drawn after every
    -- draw handler, over all panels and text, on the top layer; it shows at most max_rows (12) entries and scrolls; a
    -- click anywhere else closes it.
    function ui.dropdown(id, x, y, w, options, index, opts)
        opts = opts or {}
        local h = math.max(14, opts.height or 18)
        local handle = ui_handle(mod, id)
        local open = ui_dropdown == handle
        local r = region(id, x, y, w, h, opts)
        box(x, y, w, h, (r.hover or open) and C.bg_hover or C.bg, (r.hover or open) and C.edge or C.edge_dim, opts)
        ui.text(tostring(options[index] or ''), x + 6, y, {height = h, valign = 'middle', color = label_color(opts),
            layer = opts.layer})
        -- the arrow
        local ay = y + math.floor((h - 4) / 2)
        for i = 0, 3 do ui.rect(x + w - 14 + i, ay + i, 8 - 2 * i, 1, C.accent, opts) end
        if r.clicked then
            open = not open
            ui_dropdown = open and handle or nil
        end
        local n = #options
        local rows = math.max(1, math.min(n, opts.max_rows or 12))
        local list_id = tostring(id) .. '#list'
        if r.clicked and open then
            -- Opened: the selected entry in the middle of the list (when it has to scroll).
            local off = uis.scroll_clamp((index - 1) * 18 - (rows * 18 - 18) / 2, n * 18, rows * 18)
            scrolls[list_id] = {offset = off, content = n * 18}
        end
        local changed = false
        if open then
            -- The list's clicks (its regions were drawn last tick), so the new index is returned now.
            for i = 1, n do
                local st = ui_input[ui_handle(mod, tostring(id) .. '#' .. i)]
                if st and st.clicked then
                    changed = i ~= index
                    index = i
                    ui_dropdown = nil
                end
            end
            local away = ui_input[ui_handle(mod, tostring(id) .. '#away')]
            if away and away.clicked then ui_dropdown = nil end
        end
        if open and ui_dropdown == handle then
            -- The list itself after every draw handler: over all panels and text, and it takes the mouse first.
            local sel = index
            uis.defer(function()
                local s = ui_tick_state or {}
                local top = {layer = 'top'}
                local sl, st_, sr, sb = s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768
                -- Behind the list: the whole screen, so a click beside it closes it (and goes nowhere else).
                region(tostring(id) .. '#away', sl, st_, sr - sl, sb - st_, {layer = 'top', nav = false})
                local lh = rows * 18 + 2
                local ly = y + h
                if ly + lh > sb and y - lh >= st_ then ly = y - lh end     -- no room below: above the box
                box(x, ly, w, lh, 0xF0141420, C.edge, top)
                local area = ui.scroll_begin(list_id, x + 1, ly + 1, w - 2, rows * 18, {layer = 'top', step = 18, nav = false})
                local first = math.max(1, math.floor(area.offset / 18) + 1)
                for i = first, math.min(n, first + rows) do
                    local iy = area.content_y + (i - 1) * 18
                    local ir = region(tostring(id) .. '#' .. i, x + 1, iy, area.w, 18, {layer = 'top', _nav_drop = true})
                    if ir.hover or i == sel then
                        ui.rect(x + 1, iy, area.w, 18, ir.hover and 0xFF4A3C28 or 0xFF2A2418, top)
                    end
                    ui.text(tostring(options[i]), x + 6, iy, {height = 18, valign = 'middle', layer = 'top',
                        color = i == sel and C.accent or C.text})
                end
                ui.scroll_end(n * 18)
            end)
        end
        return changed, index
    end

    -- ui.slider(id, x, y, w, min, max, value, opts{step, format}) -> changed, value
    function ui.slider(id, x, y, w, min, max, value, opts)
        opts = opts or {}
        local function at(v) return x + (w - 10) * ((v - min) / math.max(max - min, 1e-9)) end
        local r = region(id, x, y, w, 16, {layer = opts.layer, tooltip = opts.tooltip, enabled = opts.enabled,
            _nav_kind = 1, _nav_x = at(value) + 5, _nav_y = y + 8})
        ui.rect(x, y + 7, w, 2, C.edge_dim, opts)
        local changed = false
        if ui_hot == ui_handle(mod, id) and ui_pad.nudge ~= 0 and opts.enabled ~= false then
            -- The pad: D-pad left / right on the slider, a fiftieth of its range (at least one step) at a time.
            local step = opts.step or 1
            local jump = math.max(step, math.floor((max - min) / 50 / step + 0.5) * step)
            local v = math.max(min, math.min(max, value + ui_pad.nudge * jump))
            v = math.floor(v / step + 0.5) * step
            if v ~= value then value, changed = v, true end
            ui_pad.nudge = 0
        end
        if r.held or r.pressed then
            local t = math.max(0, math.min(1, (ui_mouse.x - x - 5) / math.max(w - 10, 1)))
            local v = min + t * (max - min)
            local step = opts.step or 1
            v = math.floor(v / step + 0.5) * step
            if v ~= value then value, changed = v, true end
        end
        ui.rect(x, y + 7, at(value) - x + 5, 2, C.accent, opts)
        box(at(value), y + 2, 10, 12, (r.hover or r.held) and C.bg_down or C.bg_hover, C.edge, opts)
        if opts.format ~= false then
            ui.text((opts.format or '%g'):format(value), x + w + 6, y, {height = 16, valign = 'middle',
                color = label_color(opts), layer = opts.layer})
        end
        return changed, value
    end

    do
        ---------------------------------------------------------------------------------------------------------
        -- Tables: ui.table(id, x, y, w, opts{columns, spacing, row_height, row_heights, stripes, borders, header_height,
        -- layer}) -> table. Pure layout on top of the widgets: it resolves the columns' widths (fixed numbers, 'auto' = as wide
        -- as the widest cell of the last frame, shares of the rest with min / max), then t:row() starts a row and t:label /
        -- t:button / t:dropdown / t:checkbox / t:slider / t:image / t:cell put one thing into a column of it (spanning columns
        -- and rows, aligned and stretched as the options say). The widgets are the ordinary ones (same look, same controller
        -- stops); nothing is retained but the measures 'auto' needs, so draw it every frame and inside a scroll area use
        -- area.content_x / content_y.
        ---------------------------------------------------------------------------------------------------------
        local Table = {}
        Table.__index = Table
        local TABLE_ELLIPSIS = '...'
        local table_fit_cache, table_fit_n = {}, 0
        local table_auto = {}       -- mod's table id -> {use = last frame's {cols, rows}, new = this frame's}

        local function table_star(width, level)
            if width == nil or width == '*' then return 1 end
            local s = tostring(width)
            local n = tonumber(s:match('^(%d*%.?%d*)%*$') or '')
            if not n or n <= 0 then
                error('column width is a number, "auto", "*" or "N*" (a share of the remaining width)', level + 1)
            end
            return n
        end

        -- {x, w} of every column for a table w wide (nil: as wide as its fixed and auto columns, the shares at their
        -- minimum): fixed widths first, 'auto' ones from the measures of the last frame, then the rest shared out by the
        -- stars; a column that would pass its min / max is pinned there and the others share what is left.
        local function table_columns(columns, w, gap, auto, level)
            local n = #columns
            if n == 0 then error('a table needs columns', level + 1) end
            local specs, used = {}, gap * (n - 1)
            for i, c in ipairs(columns) do
                if type(c) ~= 'table' then c = {width = c} end
                local s = {c = c}
                if type(c.width) == 'number' then
                    s.w = c.width
                elseif c.width == 'auto' then
                    s.w = math.max(c.min or 0, math.min(c.max or math.huge, (auto and auto[i]) or 0))
                else
                    s.star = table_star(c.width, level + 1)
                end
                if s.w then used = used + s.w elseif c.min then used = used + c.min end
                specs[i] = s
            end
            if not w then w = used end
            local free = w - gap * (n - 1)
            for _, s in ipairs(specs) do if s.w then free = free - s.w end end
            local open = {}
            for _, s in ipairs(specs) do if s.star then open[#open + 1] = s end end
            while #open > 0 do
                local total = 0
                for _, s in ipairs(open) do total = total + s.star end
                local pinned, rest = false, {}
                for _, s in ipairs(open) do
                    local v = math.max(0, free) * s.star / total
                    local lo, hi = s.c.min, s.c.max
                    if lo and v < lo then s.w, pinned = lo, true; free = free - lo
                    elseif hi and v > hi then s.w, pinned = hi, true; free = free - hi
                    else rest[#rest + 1] = s end
                end
                if not pinned then
                    for _, s in ipairs(rest) do s.w = math.max(0, free) * s.star / total end
                    break
                end
                open = rest
            end
            local cols, cx = {}, 0
            for i, s in ipairs(specs) do
                -- Whole units: the edges are rounded, so neighbours never overlap or leave a hairline.
                local x0, x1 = math.floor(cx + 0.5), math.floor(cx + s.w + 0.5)
                cols[i] = {x = x0, w = x1 - x0, align = s.c.align, valign = s.c.valign, title = s.c.title}
                cx = cx + s.w + gap
            end
            return cols, w
        end

        function ui.table(id, x, y, w, opts)
            opts = opts or {}
            local gap = opts.spacing
            local gx, gy
            if type(gap) == 'table' then gx, gy = gap[1] or 8, gap[2] or 4 else gx, gy = gap or 8, gap or 4 end
            if w == 'auto' then w = nil end
            local key = tostring(id)
            local st = table_auto[key] or {}
            table_auto[key] = st
            st.use, st.new = st.new, {cols = {}, rows = {}}
            local use = st.use or {cols = {}, rows = {}}
            local b = opts.borders
            if b == true then b = {rows = true, columns = true, outer = true}
            elseif type(b) == 'table' then
                b = {rows = b.rows, columns = b.columns, outer = b.outer, color = b.color}
            else b = nil end
            local cols, tw = table_columns(opts.columns or {}, w, gx, use.cols, 2)
            local t = setmetatable({id = key, x = x, y = y, w = tw, gx = gx, gy = gy, opts = opts, borders = b,
                row_h = opts.row_height or 22, next_y = y, row_n = 0, body_n = 0, cy = y, ch = 0, cols = cols,
                state = st, use = use}, Table)
            local titled = false
            for _, c in ipairs(t.cols) do if c.title then titled = true end end
            if titled and opts.header ~= false then
                t.row_n = 1
                t.cy, t.ch = y, opts.header_height or t.row_h
                t:draw_borders(true, {})
                for i, c in ipairs(t.cols) do
                    if c.title then
                        t:label(i, c.title, {bold = true, color = C.accent, id = t.id .. '#head' .. i})
                    end
                end
                ui.rect(x, t.cy + t.ch + math.floor(gy / 2), tw, 1, C.edge_dim, {layer = opts.layer})
                t.next_y = t.cy + t.ch + gy + 1
                t.header = true
            end
            return t
        end

        -- The grid lines of the row just started (opts.borders): the column lines through the row and the gaps, a line
        -- above it (rows), the frame's sides and top (outer; the bottom is drawn by t:finish()).
        function Table:draw_borders(first, ropts)
            local b = self.borders
            if not b then return end
            local o = {layer = self.opts.layer}
            local color = b.color or C.edge_dim
            local pad = math.floor(self.gy / 2)
            local top, h = self.cy - pad, self.ch + 2 * pad
            if b.columns and ropts.columns ~= false then
                for c = 2, #self.cols do
                    ui.rect(self.x + self.cols[c].x - math.floor(self.gx / 2) - 1, top, 1, h, color, o)
                end
            end
            if b.outer then
                ui.rect(self.x, top, 1, h, color, o)
                ui.rect(self.x + self.w - 1, top, 1, h, color, o)
                if first then ui.rect(self.x, top, self.w, 1, color, o) end
            end
            if b.rows and not first and not (self.header and self.body_n == 1) then
                ui.rect(self.x, top, self.w, 1, color, o)
            end
        end

        -- t:row(height?, background?, opts?) -> row number (of the body, 1-based): the next row. `height`: a number
        -- (default the table's row_height), or 'auto' for the tallest cell of this row's last frame. `background`: an
        -- optional colour (opts.stripes shades every second row by itself). opts.columns = false: no column lines through
        -- this row (under a cell that spans columns).
        function Table:row(height, background, ropts)
            ropts = ropts or {}
            if height == 'auto' then
                height = self.use.rows[self.row_n + 1] or self.row_h
            end
            self.cy = self.next_y
            self.ch = height or (self.opts.row_heights and self.opts.row_heights[self.body_n + 1]) or self.row_h
            self.next_y = self.cy + self.ch + self.gy
            self.row_n, self.body_n = self.row_n + 1, self.body_n + 1
            local fill = background
            if not fill and self.opts.stripes and self.body_n % 2 == 0 then
                fill = self.opts.stripes == true and 0x30FFFFFF or self.opts.stripes
            end
            if fill then
                local pad = math.floor(self.gy / 2)
                ui.rect(self.x, self.cy - pad, self.w, self.ch + 2 * pad, fill, {layer = self.opts.layer})
            end
            self:draw_borders(not self.header and self.body_n == 1, ropts)
            return self.body_n
        end

        -- t:finish() -> height. Draws what needs the end (the frame's bottom line with borders.outer) and returns the
        -- table's height; the measures for 'auto' are kept as they are.
        function Table:finish()
            local b = self.borders
            if b and b.outer and self.next_y > self.y then
                local pad = math.floor(self.gy / 2)
                ui.rect(self.x, self.cy + self.ch + pad, self.w, 1, b.color or C.edge_dim, {layer = self.opts.layer})
            end
            return self:height()
        end

        -- t:column(col) -> x, w of the column (canvas coordinates).
        function Table:column(col)
            local c = self.cols[col]
            if not c then error('no column ' .. tostring(col) .. ' (the table has ' .. #self.cols .. ')', 3) end
            return self.x + c.x, c.w
        end

        -- t:cell(col, span?, rowspan?) -> x, y, w, h of the column (`span` columns from it, with the gaps between) in this
        -- row; with `rowspan` the cell reaches down over that many rows: the later rows' heights are not known yet, so they
        -- count as opts.row_heights[n] or row_height (give row_heights when the rows differ), and the cells of those rows in
        -- these columns are left to you (leave them out).
        function Table:cell(col, span, rowspan)
            local x0 = self:column(col)
            local last = col + (span or 1) - 1
            if last > #self.cols then error('the span passes the last column', 2) end
            local x1, w1 = self:column(last)
            local h = self.ch
            for k = 1, (rowspan or 1) - 1 do
                local rh = self.opts.row_heights and self.opts.row_heights[self.body_n + k] or self.row_h
                h = h + self.gy + rh
            end
            return x0, self.cy, x1 + w1 - x0, h
        end

        -- t:height() -> the height of everything so far (the header and the rows, without a trailing gap); the content
        -- height for scroll_end. t:width() is the table's width (its columns' sum when it was made without one).
        function Table:height()
            if self.next_y == self.y then return 0 end
            return self.next_y - self.gy - self.y
        end
        function Table:width() return self.w end

        -- Notes the natural size of a cell for the next frame's 'auto' columns and rows (a cell that spans does not count).
        function Table:measure(col, o, nw, nh)
            if (o.span or 1) == 1 and nw then
                local cur = self.state.new.cols[col]
                if not cur or nw > cur then self.state.new.cols[col] = nw end
            end
            if (o.rowspan or 1) == 1 and nh then
                local cur = self.state.new.rows[self.row_n]
                if not cur or nh > cur then self.state.new.rows[self.row_n] = nh end
            end
        end

        -- The place of a widget in its cell. Horizontally opts.halign (alias align; else the column's align): 'left',
        -- 'center', 'right', or 'stretch' (the cell's width); vertically opts.valign (else the column's): 'top', 'center'
        -- ('middle'), 'bottom' or 'stretch'. The default is stretch for what has no size of its own (buttons, dropdowns) unless
        -- opts.width / opts.height are given, else left and centred. opts.width / height / `nw` / `nh` set the size otherwise.
        function Table:place(col, o, nw, nh)
            local x, y, w, h = self:cell(col, o.span, o.rowspan)
            local c = self.cols[col]
            local ww, hh = o.width or nw, o.height or nh
            local ha = o.halign or o.align or (ww and (c.align or 'left') or 'stretch')
            local va = o.valign or c.valign or (hh and 'center' or 'stretch')
            if va == 'middle' then va = 'center' end
            if ha == 'stretch' or not ww then ww = w else ww = math.min(w, ww) end
            if va == 'stretch' or not hh then hh = h end
            local px, py = x, y
            if ha == 'right' then px = x + w - ww elseif ha == 'center' then px = x + math.floor((w - ww) / 2) end
            if va == 'bottom' then py = y + h - hh elseif va == 'center' then py = y + math.floor((h - hh) / 2) end
            return px, py, ww, hh
        end

        -- A copy of the widget's options with the table's layer and an automatic id.
        function Table:wopts(col, opts)
            local o = {}
            for k, v in pairs(opts or {}) do o[k] = v end
            o.layer = o.layer or self.opts.layer
            o.id = o.id or (self.id .. '#' .. self.row_n .. ',' .. col)
            return o
        end

        -- Text cut to `w` with "..." (cached).
        local function table_fit(text, w, mo)
            local tw = ui_measure(text, mo)
            if not tw or tw <= w then return text, tw end
            local key = table.concat({text, w, mo.size or 0, mo.face or '', mo.bold and 1 or 0, mo.italic and 1 or 0}, '\1')
            local hit = table_fit_cache[key]
            if hit then return hit[1], hit[2] end
            local lo, hi = 0, #text
            while lo < hi do
                local mid = math.ceil((lo + hi) / 2)
                -- never cut inside a UTF-8 sequence
                while mid > 0 and (text:byte(mid + 1) or 0) >= 0x80 and (text:byte(mid + 1) or 0) < 0xC0 do mid = mid - 1 end
                if mid <= lo then break end
                local mw = ui_measure(text:sub(1, mid) .. TABLE_ELLIPSIS, mo)
                if mw and mw <= w then lo = mid else hi = mid - 1 end
            end
            local cut = text:sub(1, lo) .. TABLE_ELLIPSIS
            local cw = ui_measure(cut, mo)
            if table_fit_n > 512 then table_fit_cache, table_fit_n = {}, 0 end
            table_fit_cache[key], table_fit_n = {cut, cw}, table_fit_n + 1
            return cut, cw
        end

        -- t:label(col, text, opts) -> drawn. opts: the label's (font, size, bold, color, tooltip, ...), halign (left, center,
        -- right; stretch is left), valign (top, center, bottom; default centre), span, rowspan, ellipsis (default true: too
        -- long a text is cut with "..." and shows whole as the tooltip).
        function Table:label(col, text, opts)
            local o = self:wopts(col, opts)
            local x, y, w, h = self:cell(col, o.span, o.rowspan)
            text = tostring(text)
            local full, fh = ui_measure(text, o)
            self:measure(col, o, full and math.ceil(full), fh and math.ceil(fh) + 4)
            local shown, tw = text, full
            if o.ellipsis ~= false then shown, tw = table_fit(text, w, o) end
            local ha = o.halign or o.align or self.cols[col].align or 'left'
            local px = x
            if tw and ha == 'right' then px = x + w - tw elseif tw and ha == 'center' then px = x + math.floor((w - tw) / 2) end
            local va = o.valign or self.cols[col].valign or 'center'
            o.valign = (va == 'stretch' or va == 'center') and 'middle' or va
            o.height = o.height or h
            if o.tooltip == nil and shown ~= text then o.tooltip = text end
            return ui.label(o.id, shown, px, y, o)
        end

        -- t:button(col, text, opts) -> clicked. Fills the cell unless opts.width / height / halign / valign say otherwise.
        function Table:button(col, text, opts)
            local o = self:wopts(col, opts)
            local tw, th = ui_measure(tostring(text), {})
            self:measure(col, o, (tw and math.ceil(tw) or 0) + 16 + (o.image and 18 or 0), th and math.ceil(th) + 8)
            local x, y, w, h = self:place(col, o)
            return ui.button(o.id, x, y, w, h, text, o)
        end

        -- t:checkbox(col, text, checked, opts) -> changed, checked
        function Table:checkbox(col, text, checked, opts)
            local o = self:wopts(col, opts)
            local tw = ui_measure(tostring(text), {}) or 60
            self:measure(col, o, 20 + math.ceil(tw), 16)
            local x, y = self:place(col, o, 20 + tw, 16)
            return ui.checkbox(o.id, x, y, text, checked, o)
        end

        -- t:dropdown(col, options, index, opts) -> changed, index. As high as the row (opts.height otherwise).
        function Table:dropdown(col, options, index, opts)
            local o = self:wopts(col, opts)
            local widest = 0
            for _, text in ipairs(options) do widest = math.max(widest, ui_measure(tostring(text), {}) or 0) end
            self:measure(col, o, math.ceil(widest) + 26, o.height or 18)
            local x, y, w, h = self:place(col, o)
            o.height = h
            return ui.dropdown(o.id, x, y, w, options, index, o)
        end

        -- t:slider(col, min, max, value, opts) -> changed, value. The number (unless opts.format == false) takes
        -- opts.value_width (default 44) at the cell's right end; with opts.width the slider is that wide and the number lies beside it.
        function Table:slider(col, min, max, value, opts)
            local o = self:wopts(col, opts)
            self:measure(col, o, nil, 16)
            local x, y, w = self:place(col, o, nil, 16)
            if o.format ~= false and not o.width then w = math.max(10, w - (o.value_width or 44)) end
            return ui.slider(o.id, x, y, w, min, max, value, o)
        end

        -- t:image(col, image, opts) -> drawn. opts: ui.image's (fit defaults to 'contain'), width, height, halign, valign.
        function Table:image(col, image, opts)
            local o = self:wopts(col, opts)
            self:measure(col, o, o.width, o.height)
            local x, y, w, h = self:place(col, o)
            o.fit = o.fit or 'contain'
            return ui.image(image, x, y, w, h, o)
        end
    end

    -- ui.panel(id, title, x, y, w, h, opts{closable, draggable, layer, bare}) -> panel {x, y, w, h, open, content_x,
    -- content_y}. A window of the mod's: the title bar drags it (it keeps the position for the session), its X
    -- closes it (panel.open false; ui.show_panel(id) opens it again). Its body takes the mouse, so clicks on it do
    -- not reach the game.
    -- Panels: a mod's windows. The layout keeps each one's place (and size, if resizable) under "mod_id:id".
    local panels = {}
    -- The open / closed state is kept in the layout (mods\gear_layout.json, "open": true / false) and comes back at the next
    -- start, after the mod's main.lua ran (its own show_panel calls there are the defaults). ui.persist_panel(id, false)
    -- takes a panel out (for screens that only make sense with a state of the mod: a dialog, a keyboard).
    local function remember_open(id, open)
        local p = panels[id]
        if mod.loading or (p and p.nopersist) then return end
        local key = mod.id .. ':' .. tostring(id)
        local lay = layout_get()
        local e = lay[key]
        if not e then e = {}; lay[key] = e end
        if e.open ~= open then e.open = open; layout_dirty = true end
    end
    function ui.show_panel(id, open)
        local p = panels[id] or {}
        panels[id] = p
        p.closed = open == false
        remember_open(id, not p.closed)
    end
    function ui.persist_panel(id, on)
        local p = panels[id] or {}
        panels[id] = p
        p.nopersist = on == false or nil
        if on == false then
            local e = layout_get()[mod.id .. ':' .. tostring(id)]
            if e and e.open ~= nil then
                e.open = nil
                if next(e) == nil then layout_get()[mod.id .. ':' .. tostring(id)] = nil end
                layout_dirty = true
            end
        end
    end
    -- Called by the kernel after main.lua: the kept states win over the defaults the mod set.
    function mod.restore_panels()
        local prefix = mod.id .. ':'
        for key, e in pairs(layout_get()) do
            if key:sub(1, #prefix) == prefix and type(e) == 'table' and e.open ~= nil then
                local id = key:sub(#prefix + 1)
                local p = panels[id] or {}
                panels[id] = p
                if not p.nopersist then p.closed = e.open == false end
            end
        end
    end
    function ui.panel_open(id)
        return not (panels[id] and panels[id].closed)
    end
    -- ui.toggle_panel(id) -> open: shows a hidden panel, hides a shown one.
    function ui.toggle_panel(id)
        local open = not ui.panel_open(id)
        ui.show_panel(id, open)
        return open
    end
    function ui.panel(id, title, x, y, w, h, opts)
        opts = opts or {}
        local p = panels[id] or {}
        panels[id] = p
        if p.closed then return {open = false} end
        local key = mod.id .. ':' .. tostring(id)
        local saved = layout_get()[key]
        if saved and not saved.x then saved = nil end
        if saved then
            x, y = from_virtual(saved.x, saved.y)
            if opts.resizable and saved.w then w, h = size_from_virtual(saved.w, saved.h) end
        end
        if p.x then x, y = p.x, p.y end         -- being dragged
        if p.w then w, h = p.w, p.h end         -- being resized
        local min_w, min_h = opts.min_width or 120, opts.min_height or 60
        w, h = math.max(w, min_w), math.max(h, min_h)
        if saved and not p.x then
            -- A kept place (a fraction of the screen, see from_virtual) or a size from another screen shape may not fit
            -- this one: the panel stays on the screen (left / top win when it is larger than the screen).
            local s = ui_tick_state or {}
            local sl, st_, sr, sb = s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768
            x = math.max(sl, math.min(x, sr - w))
            y = math.max(st_, math.min(y, sb - h))
        end
        local o = {layer = opts.layer}
        if opts.bare then
            -- No window: nothing drawn (the layout editor's mode shows the frame), no region (the mouse and clicks go on to the
            -- game), the mod draws in the whole rectangle. The layout editor still moves and, with opts.resizable, sizes it.
            if ui_edit then ui.frame(x, y, w, h, C.accent, 1, o) end
            ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = id, title = tostring(title), x = x, y = y,
                w = w, h = h, closable = false, back = false, resizable = opts.resizable and true or false,
                min_width = min_w, min_height = min_h, layer = opts.layer or 'hud'}
            return {open = true, key = key, x = x, y = y, w = w, h = h, content_x = x, content_y = y, content_w = w, content_h = h}
        end
        ui.rect(x, y, w, h, opts.background or 0xD80E0E14, o)
        ui.frame(x, y, w, h, ui_edit and C.accent or C.edge, ui_edit and 2 or 1, o)
        ui.rect(x + 1, y + 1, w - 2, 20, C.title, o)
        ui.text(title, x + 8, y + 1, {height = 20, valign = 'middle', color = C.accent, layer = opts.layer})
        region(tostring(id) .. '#body', x, y, w, h, {layer = opts.layer, nav = false})
        local function store()
            local vx, vy = to_virtual(x, y)
            local e = {x = vx, y = vy}
            if opts.resizable then e.w, e.h = size_to_virtual(w, h) end
            layout_set(key, e)
        end
        if opts.draggable ~= false then
            local bar = region(tostring(id) .. '#bar', x, y, w - 24, 21, {layer = opts.layer, tooltip = opts.tooltip, nav = false})
            if bar.pressed then p.grab_x, p.grab_y = ui_mouse.x - x, ui_mouse.y - y end
            if bar.held and p.grab_x then
                p.x, p.y = ui_mouse.x - p.grab_x, ui_mouse.y - p.grab_y
                x, y = p.x, p.y
            elseif p.x then
                store()
                p.x, p.y, p.grab_x = nil, nil, nil
            end
        end
        if opts.resizable then
            local grip = region(tostring(id) .. '#grip', x + w - 14, y + h - 14, 14, 14, {layer = opts.layer, tooltip = 'Resize', nav = ui_edit})
            for i = 0, 2 do
                ui.rect(x + w - 4 - i * 4, y + h - 4, 2, 2, grip.hover and C.accent or C.edge, o)
                ui.rect(x + w - 4, y + h - 4 - i * 4, 2, 2, grip.hover and C.accent or C.edge, o)
            end
            if grip.pressed then p.grab_w, p.grab_h = ui_mouse.x - w, ui_mouse.y - h end
            if grip.held and p.grab_w then
                p.w, p.h = math.max(min_w, ui_mouse.x - p.grab_w), math.max(min_h, ui_mouse.y - p.grab_h)
            elseif p.w then
                w, h = p.w, p.h
                store()
                p.w, p.h, p.grab_w = nil, nil, nil
            end
        end
        if opts.closable ~= false then
            local cr = region(tostring(id) .. '#close', x + w - 21, y + 2, 18, 18, {layer = opts.layer, tooltip = 'Close'})
            ui.text('x', x + w - 21, y + 2, {width = 18, height = 18, align = 'center', valign = 'middle',
                color = cr.hover and 0xFFFF8080 or C.text, layer = opts.layer})
            if cr.clicked then p.closed = true; remember_open(id, false) end
            -- The pad's B closes the topmost closable panel (opts.back == false: the mod handles B itself).
            if opts.back ~= false and not opts.passive then
                ui_window_open = true
                if ui_pad.back > 0 then
                    local top = nil
                    for _, q in ipairs(ui_panel_prev) do
                        if q.closable and q.back then top = q.key end
                    end
                    if top == key or (top == nil) then
                        p.closed = true
                        remember_open(id, false)
                        ui_pad.back = ui_pad.back - 1
                    end
                end
            end
        end
        ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = id, title = tostring(title), x = x, y = y,
            w = w, h = h, closable = opts.closable ~= false, back = opts.back ~= false and not opts.passive,
            resizable = opts.resizable and true or false, min_width = min_w, min_height = min_h,
            layer = opts.layer or 'hud'}
        return {open = true, key = key, x = x, y = y, w = w, h = h, content_x = x + 8, content_y = y + 28,
            content_w = w - 16, content_h = h - 36}
    end

    -- Layout editing (like an "edit mode" in other games): the panels of every mod show their ids and their resize
    -- grips while it is on; a layout editor (the layout mod of sacred-gear-mods) moves the game's windows too.
    -- ui.nav_modal(on): the mod's screen takes the controller alone while on: the pad's D-pad only reaches its
    -- widgets (and nothing else), B is ui.back_pressed(), and the mod menu button is not offered.
    function ui.nav_modal(on)
        if on then ui_nav_modal = mod elseif ui_nav_modal == mod then ui_nav_modal = nil end
    end
    -- ui.replace_window(name, on): the mod draws its own screen in place of the game's window `name` (in game a
    -- control name such as UI_WND_SAVEGAME, in the menus UI_MENU_LOADGAME ...): while it is open, the game's window
    -- is not drawn and gets no mouse or keys. -> true | false, message. Each tick it shows its screen the mod calls
    -- ui.replaced(name); without that the game's window comes back (1.5 s after it opened, 0.5 s after the last).
    function ui.replace_window(name, on)
        name = tostring(name):upper()
        if not native.ui_replace_known(name) then return false, 'no window ' .. name end
        local owner = uis.replace[name]
        if on == false then
            if owner == mod then uis.replace[name] = nil end
            return true
        end
        if owner and owner ~= mod and not owner.disabled and not owner.unloaded then
            return false, name .. ' is replaced by ' .. tostring(owner.id)
        end
        uis.replace[name] = mod
        return true
    end
    -- ui.replaced(name) -> bool: the game's window is open and the mod's screen takes its place now (call it every
    -- tick the screen is drawn; false: draw nothing, the window is closed or the game's own shows).
    function ui.replaced(name)
        name = tostring(name):upper()
        if uis.replace[name] ~= mod then return false end
        return native.ui_replace_beat(name)
    end
    -- ui.window_open(name) -> bool: whether the game's window (or menu screen) is open.
    function ui.window_open(name) return native.ui_window_open(tostring(name):upper()) end
    -- ui.close_window(name) -> bool: closes the game's window as its own Back (or Esc) does.
    function ui.close_window(name) return native.ui_close_window(tostring(name):upper()) end
    -- ui.menu_entries(name) -> {{text, x, y, w, h}, ...}: the entries of the game menu ('UI_WND_ESCMENU') in their order, with the game's own
    -- text (in its language) and rectangles; empty while the menu is closed. ui.menu_click(name, index) presses entry `index` (1-based)
    -- as the game's own click would, also while a mod replaces the menu (ui.replace_window).
    function ui.menu_entries(name) return native.ui_menu_entries and native.ui_menu_entries(tostring(name):upper()) or {} end
    function ui.menu_click(name, index) return native.ui_menu_click and native.ui_menu_click(tostring(name):upper(), index) or false end
    -- ui.back_pressed() -> bool: the controller's B was pressed (consumed: true once per press). Mods with a
    -- nav_modal screen or a panel with `back = false` close or step back with it.
    function ui.back_pressed()
        if ui_pad.back > 0 then
            ui_pad.back = ui_pad.back - 1
            return true
        end
        return false
    end
    -- ui.menu_item(id, label, fn): an entry of the controller's mod menu (the pad button reaches it where a mod's
    -- tool needs a hotkey, ALT or a click on a tiny button). Declared keys (mod.lua) are listed there on their own.
    function ui.menu_item(id, label, fn)
        assert(type(fn) == 'function', 'menu_item wants a function')
        local items = ui_menu_items[mod] or {}
        ui_menu_items[mod] = items
        for _, it in ipairs(items) do
            if it.id == id then it.label, it.fn = tostring(label), fn return end
        end
        items[#items + 1] = {id = id, label = tostring(label), fn = fn}
    end
    function ui.edit_mode() return ui_edit end
    function ui.set_edit_mode(on) ui_edit = on and true or false end
    function ui.can_move_windows() return native.ui_can_move_windows() end
    -- The panels of every mod drawn this tick (those not drawn yet: as of the last tick), in draw order:
    -- {key, mod, id, title, x, y, w, h, resizable, min_width, min_height, layer}.
    local function find_panel(key)
        for _, list in ipairs({ui_panel_list, ui_panel_prev}) do
            for _, e in ipairs(list) do
                if e.key == key then return e end
            end
        end
    end
    function ui.panels()
        local out, seen = {}, {}
        for _, list in ipairs({ui_panel_prev, ui_panel_list}) do
            for _, e in ipairs(list) do
                if not seen[e.key] then
                    seen[e.key] = #out + 1
                    out[#out + 1] = copy(e)
                else
                    out[seen[e.key]] = copy(e)      -- this tick's place wins
                end
            end
        end
        return out
    end
    -- Puts panel `key` ("mod_id:panel_id", any mod's) at (x, y) of the canvas, and, if it is resizable, makes it w x h
    -- (w, h nil: its size stays); kept in the layout. The panel takes it the next time its mod draws it.
    function ui.set_panel_rect(key, x, y, w, h)
        local e = find_panel(key)
        if not e then return false end
        local vx, vy = to_virtual(x, y)
        local entry = {x = vx, y = vy}
        if e.resizable then
            w = math.max(w or e.w, e.min_width)
            h = math.max(h or e.h, e.min_height)
            entry.w, entry.h = size_to_virtual(w, h)
        end
        layout_set(key, entry)
        e.x, e.y = x, y
        if e.resizable then e.w, e.h = w, h end
        return true
    end
    -- Panel `key` back to the place (and size) its mod gives it.
    function ui.reset_panel(key)
        if layout_get()[key] then layout_set(key, nil) end
        return find_panel(key) ~= nil
    end
    -- ui.capture_keys(on): while on, the keyboard does not reach the game (key messages are dropped, the game's key
    -- queries see nothing held); this mod's own key_pressed / key_down still work. Released when the mod unloads.
    function ui.capture_keys(on)
        key_capture[mod] = on and true or nil
    end
    -- ui.typed() -> string: what the player typed since the last tick while this mod captures the keys (UTF-8, in
    -- the player's keyboard layout: umlauts, Shift, AltGr; no control characters: read Backspace and Enter with
    -- key_pressed). For a text field.
    function ui.typed()
        return key_capture[mod] and uis.typed or ''
    end
    -- ui.key_down(name): whether the key is held (while the game has the focus).
    function ui.key_down(name)
        local vk = type(name) == 'number' and name or UI_VK[tostring(name):lower()]
        if not vk then error('unknown key ' .. tostring(name), 2) end
        return native.key_down(vk)
    end
    -- The game window `name` (a control name) with its top-left corner at (x, y) of the canvas; kept in the layout.
    function ui.set_window_position(name, x, y)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() and w.movable then
                if not native.ui_move_window(w.slot, x - w.x, y - w.y, true) then return false end
                local vx, vy = to_virtual(x, y)
                layout_set('game:' .. w.name, {x = vx, y = vy})
                return true
            end
        end
        return false
    end
    -- Back to its own place (and out of the layout).
    function ui.reset_window(name)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() then
                native.ui_move_window(w.slot, 0, 0, false)
                if layout_get()['game:' .. w.name] then layout_set('game:' .. w.name, nil) end
                return true
            end
        end
        return false
    end
    -- Forgets the places of all panels and game windows (or the ones whose key starts with `prefix`).
    function ui.reset_layout(prefix)
        for key in pairs(layout_get()) do
            if not prefix or key:sub(1, #prefix) == prefix then
                if key:sub(1, 5) == 'game:' then ui.reset_window(key:sub(6)) else layout_set(key, nil) end
            end
        end
        for _, p in pairs(panels) do p.x, p.y, p.w, p.h = nil, nil, nil, nil end
    end
    -- The stored layout: {key = {x, y, w, h}} in the virtual 4096 x 4096 screen.
    function ui.layout()
        local out = {}
        for k, v in pairs(layout_get()) do
            if v.x then out[k] = {x = v.x, y = v.y, w = v.w, h = v.h, open = v.open} end
        end
        return out
    end
    function ui.to_virtual(x, y) return to_virtual(x, y) end
    function ui.from_virtual(vx, vy) return from_virtual(vx, vy) end

    -- ui.text_size(text, font_or_opts): a font (see ui.text) or a table of ui.text's font options.
    function ui.text_size(text, font)
        return ui_measure(tostring(text), type(font) == 'table' and font or {font = font})
    end
    function ui.windows() return native.ui_windows() end
    function ui.window(name)
        for _, w in ipairs(native.ui_windows()) do
            if w.name:upper() == tostring(name):upper() then return w end
        end
        return nil
    end
    function ui.state() return native.ui_state() end
    -- ui.tooltip_rect(w, h, opts) -> x, y: where to put a w x h tooltip (or any popup beside the cursor) so that it is
    -- clear of the game's cursor sprite and inside the screen. opts: x, y (the point to avoid, default the cursor),
    -- bounds {left, top, right, bottom} (default the screen), cursor = false (the point has no sprite: only the gap).
    function ui.tooltip_rect(w, h, opts)
        assert(type(w) == 'number' and type(h) == 'number', 'w and h must be numbers')
        opts = opts or {}
        assert(type(opts) == 'table', 'opts must be a table')
        local s = native.ui_state()
        local b = opts.bounds
        if b ~= nil then
            assert(type(b) == 'table' and #b == 4, 'bounds must be {left, top, right, bottom}')
        else
            b = {s.screen_left or 0, s.screen_top or 0, s.screen_right or 1024, s.screen_bottom or 768}
        end
        local mx = opts.x or ui_mouse.x
        local my = opts.y or ui_mouse.y
        local cw, ch = UI_CURSOR_W, UI_CURSOR_H
        if opts.cursor == false then cw, ch = 0, 0 end
        return tooltip_place(w, h, mx, my, b, cw, ch, 4)
    end
    function ui.screen()
        local s = native.ui_state()
        return s.screen_left, s.screen_top, s.screen_right, s.screen_bottom
    end
    return ui
end

---------------------------------------------------------------------------------------------------------------
-- Custom events (gear.emit). Built-in events cannot be emitted by mods.
---------------------------------------------------------------------------------------------------------------
local BUILTIN = {load = true, frame = true, idle = true, draw = true, unload = true, damage = true, ['effect.add'] = true}
local RESERVED_PREFIX = {'session.', 'gear.', 'pipe.', 'game.', 'realm.', 'settings.', 'level.', 'combat.', 'creature.', 'bonus.', 'skillslot.', 'chat.', 'party.', 'buff.'}
-- EV: emit depth and the hook registry (gear.events: declared custom events, listed for other mods and the docs).
local EV = {depth = 0, declared = {}}

function EV.check(event)
    assert(type(event) == 'string' and event:match('^[%w_%-]+%.[%w_%.%-]+$'),
        'custom event names have a dot ("twitch.command")')
    assert(not BUILTIN[event], 'built-in event')
    for _, p in ipairs(RESERVED_PREFIX) do
        assert(event:sub(1, #p) ~= p, 'events starting with ' .. p .. ' are reserved')
    end
end

-- Handlers of `prefix.*` ("bounty.*", "bounty.board.*") also get every custom event below that prefix, with the event name first.
local function emit(event, ...)
    EV.check(event)
    if EV.depth >= 8 then error('events nested too deeply', 2) end
    EV.depth = EV.depth + 1
    local ok, err = pcall(function(...)
        kernel.dispatch(event, ...)
        local dot = 0
        while true do
            dot = event:find('.', dot + 1, true)
            if not dot then break end
            kernel.dispatch(event:sub(1, dot) .. '*', event, ...)
        end
    end, ...)
    EV.depth = EV.depth - 1
    if not ok then error(err, 0) end
end

-- gear.events.declare(name, info): the mod says it emits `name`. info = {doc = 'when', args = 'payload fields'}.
function EV.declare(mod, name, info)
    EV.check(name)
    assert(not name:find('*', 1, true), 'declare the event, not a pattern')
    info = type(info) == 'table' and info or {doc = info}
    local old = EV.declared[name]
    assert(not old or old.mod == mod.id, ('%s is declared by mod %s'):format(name, old and old.mod))
    EV.declared[name] = {name = name, mod = mod.id, doc = tostring(info.doc or ''), args = tostring(info.args or '')}
end

function EV.list(prefix)
    local out = {}
    for name, d in pairs(EV.declared) do
        if not prefix or name:sub(1, #prefix) == prefix then
            local n = 0
            for _, h in ipairs(handlers[name] or {}) do if not h.mod.disabled then n = n + 1 end end
            out[#out + 1] = {name = name, mod = d.mod, doc = d.doc, args = d.args, listeners = n}
        end
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

---------------------------------------------------------------------------------------------------------------
-- Sessions: 'session.start' (hero) when a hero appears, 'session.stop' when it goes or another one replaces it.
-- The frame tick notices, so a session that ends in the menu stops with the next game's first frame.
---------------------------------------------------------------------------------------------------------------
local session_hero

local function check_session()
    local h = native.hero()
    if h == session_hero then return end
    if session_hero then
        session_hero = nil
        kernel.dispatch('session.stop')
        labels = {}
        kernel.boss.reset()
    end
    if h then
        session_hero = h
        kernel.dispatch('session.start', wrap(h))
    end
end


---------------------------------------------------------------------------------------------------------------
-- Tasks: gear.task(fn) runs fn as a coroutine that may wait (gear.wait(seconds), gear.wait_until(test, timeout))
-- without holding up the frame. Resumed every tick (in game every frame; with automation in the menus too).
---------------------------------------------------------------------------------------------------------------
local tasks = {}        -- array of {mod, co, wake (time), test, deadline}
local WAIT = {}         -- the marker a waiting task yields

local function start_task(mod, fn)
    assert(type(fn) == 'function', 'task must be a function')
    local t = {mod = mod, co = coroutine.create(fn), wake = 0}
    tasks[#tasks + 1] = t
    return t
end

local running_task      -- the task being resumed

local function in_task()
    local co = coroutine.running()
    if running_task and co == running_task.co then return running_task end
end

-- gear.wait(seconds): only in a task.
local function wait(seconds)
    assert(type(seconds) == 'number' and seconds >= 0, 'seconds must be a number >= 0')
    local t = in_task()
    if not t then error('gear.wait only works inside gear.task', 2) end
    t.wake = native.time() + seconds
    t.test, t.deadline = nil, nil
    coroutine.yield(WAIT)
end

-- gear.wait_until(test, timeout) -> true when test() returned true, false after `timeout` seconds (default 30).
local function wait_until(test, timeout)
    assert(type(test) == 'function', 'test must be a function')
    local t = in_task()
    if not t then error('gear.wait_until only works inside gear.task', 2) end
    if test() then return true end
    t.wake = 0
    t.test = test
    t.deadline = native.time() + (timeout or 30)
    return coroutine.yield(WAIT)
end

local function run_tasks()
    if #tasks == 0 then return end
    local now = native.time()
    local list = tasks
    tasks = {}
    local keep = {}
    for _, t in ipairs(list) do
        local resume, arg = false, nil
        if t.mod.disabled then
            -- dropped
        elseif t.test then
            local ok, done = call(t.mod, 'task condition', t.test)
            if not ok then
                t.dead = true
            elseif done then
                resume, arg = true, true
            elseif now >= t.deadline then
                resume, arg = true, false
            end
        else
            resume = now >= t.wake
        end
        if resume and not t.dead then
            t.test, t.deadline = nil, nil
            local previous = current
            current = t.mod
            running_task = t
            native.reset_budget()
            local ok, err = coroutine.resume(t.co, arg)
            running_task = nil
            current = previous
            if not ok then
                t.dead = true
                t.mod.errors = t.mod.errors + 1
                log('[%s] error in task: %s', t.mod.id, debug.traceback(t.co, tostring(err)))
                if t.mod.errors >= MAX_ERRORS and not t.mod.disabled then
                    t.mod.disabled = true
                    log('[%s] %d errors: the mod is switched off until the game restarts', t.mod.id, t.mod.errors)
                end
            end
        end
        if not t.dead and not t.mod.disabled and coroutine.status(t.co) ~= 'dead' then
            keep[#keep + 1] = t
        end
    end
    -- Tasks started while these ran are in `tasks` now.
    for _, t in ipairs(tasks) do keep[#keep + 1] = t end
    tasks = keep
end

---------------------------------------------------------------------------------------------------------------
-- Automation (gear.auto): only with the game started with -gear-auto, and only for mods whose mod.lua says
-- `automation = true`. Positions: `x, y` in client pixels for move/click; ui_click takes UI layout coordinates
-- (what ui() reports) and maps them to pixels by probing the game's cursor (works with SacredBild's scaled UI).
---------------------------------------------------------------------------------------------------------------
local automation_on = native.auto_state ~= nil
local VK = {
    lbutton = 0x01, rbutton = 0x02, mbutton = 0x04, xbutton1 = 0x05, xbutton2 = 0x06, back = 0x08, backspace = 0x08, tab = 0x09, enter = 0x0D, shift = 0x10,
    control = 0x11, alt = 0x12, pause = 0x13, escape = 0x1B, space = 0x20, left = 0x25, up = 0x26, right = 0x27,
    down = 0x28, delete = 0x2E, insert = 0x2D, home = 0x24, ['end'] = 0x23, pageup = 0x21, pagedown = 0x22, f1 = 0x70, f2 = 0x71, f3 = 0x72, f4 = 0x73, f5 = 0x74, f6 = 0x75, f7 = 0x76,
    f8 = 0x77, f9 = 0x78, f10 = 0x79, f11 = 0x7A, f12 = 0x7B,
}
for i = 13, 24 do VK['f' .. i] = 0x6F + i end
for c = string.byte('A'), string.byte('Z') do VK[string.char(c):lower()] = c end
for c = string.byte('0'), string.byte('9') do VK[string.char(c)] = c end

local function vk_of(key)
    if type(key) == 'number' then return key end
    assert(type(key) == 'string', 'key must be a name or a virtual key code')
    local v = VK[key:lower()]
    if not v then error('unknown key ' .. key, 3) end
    return v
end

local ui_map            -- {ax, bx, ay, by}: layout = a * pixel + b, from two cursor probes

local function make_auto()
    local auto = {VK = VK, instance = native.auto_instance()}
    function auto.state() return native.auto_state() end
    function auto.ui() return native.auto_ui() end
    function auto.move(x, y)
        assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
        native.auto_move(math.floor(x), math.floor(y))
    end
    function auto.down(button) native.auto_button(vk_of(button or 'lbutton'), true) end
    -- auto.wheel(notches): the mouse wheel at the cursor (+ away from the player: scrolls up), as WM_MOUSEWHEEL.
    function auto.wheel(notches) native.auto_wheel(math.floor((notches or 1) * 120 + 0.5)) end
    function auto.up(button) native.auto_button(vk_of(button or 'lbutton'), false) end
    function auto.key_down(key) native.auto_key(vk_of(key), true) end
    function auto.key_up(key) native.auto_key(vk_of(key), false) end
    function auto.release() native.auto_release() end
    -- The controller, without one: pad events as SacredBild posts them (slider steps, B, the mod menu button), and
    -- the stops SacredBild holds (count, flags; count -1 stale, -2 no SacredBild).
    function auto.pad(nudge, back, menu) native.auto_pad(nudge or 0, back or 0, menu or 0) end
    function auto.nav() return native.auto_nav() end
    -- In a task: press and release with a short pause, as a player would.
    function auto.click(x, y, button)
        auto.move(x, y)
        wait(0.05)
        auto.down(button)
        wait(0.08)
        auto.up(button)
        wait(0.05)
    end
    function auto.press(key)
        auto.key_down(key)
        wait(0.08)
        auto.key_up(key)
        wait(0.05)
    end
    function auto.type(text)
        assert(type(text) == 'string', 'text must be a string')
        for i = 1, #text do
            native.auto_char(text:byte(i))
            wait(0.02)
        end
    end
    -- In a task: the client pixel for UI layout point (x, y).
    function auto.ui_to_pixels(x, y)
        if not ui_map then
            local s = native.auto_state()
            local w, h = s.width, s.height
            if w <= 0 or h <= 0 then return nil, 'no game window' end
            local px1, py1, px2, py2 = math.floor(w * 0.25), math.floor(h * 0.25), math.floor(w * 0.75), math.floor(h * 0.75)
            auto.move(px1, py1)
            wait(0.15)
            local a = native.auto_state()
            auto.move(px2, py2)
            wait(0.15)
            local b = native.auto_state()
            if a.mouse_x == b.mouse_x or a.mouse_y == b.mouse_y then
                return nil, 'the game cursor does not follow (is the window up?)'
            end
            local ax = (b.mouse_x - a.mouse_x) / (px2 - px1)
            local ay = (b.mouse_y - a.mouse_y) / (py2 - py1)
            ui_map = {ax = ax, bx = a.mouse_x - ax * px1, ay = ay, by = a.mouse_y - ay * py1}
        end
        return math.floor((x - ui_map.bx) / ui_map.ax + 0.5), math.floor((y - ui_map.by) / ui_map.ay + 0.5)
    end
    -- auto.widget(id, mod_id?) -> x, y, w, h: where a widget (the region of ui.button and the rest, by the id it was
    -- given) was drawn in the last tick, in UI layout coordinates (clipped to a scroll area); nil when it was not drawn.
    function auto.widget(id, mod_id)
        id, mod_id = tostring(id), mod_id or mod.id
        for _, r in ipairs(ui_prev_regions) do
            if r.id == id and r.mod == mod_id then return r.x, r.y, r.w, r.h end
        end
    end
    -- In a task: click the UI layout point (x, y), or the center of a node from ui().
    function auto.ui_click(x, y, button)
        if type(x) == 'table' then
            x, y, button = x.x + x.width / 2, x.y + x.height / 2, y
        end
        local px, py = auto.ui_to_pixels(x, y)
        if not px then return nil, py end
        auto.click(px, py, button)
        return true
    end
    function auto.load_save(slot)
        assert(type(slot) == 'number', 'slot must be a number')
        return native.auto_load(slot)
    end
    function auto.screenshot(name)
        assert(type(name) == 'string', 'name must be a string')
        if not name:lower():match('%.bmp$') then name = name .. '.bmp' end
        return native.auto_screenshot(name)
    end
    function auto.write(name, text)
        assert(type(name) == 'string' and type(text) == 'string', 'name and text must be strings')
        return native.auto_write(name, text)
    end
    -- Starts the engine's own level function on a worker thread (it answers through the level.factor hook when a mod handles
    -- it); false if one is running or the function was not found. `engine_level_factor_result` gives the integer once done.
    function auto.engine_level_factor_start(level)
        assert(type(level) == 'number', 'level must be a number')
        return native.engine_level_factor_start(level)
    end
    function auto.engine_level_factor_result() return native.engine_level_factor_result() end
    -- The number the game's own "first free savegame number" gives now (nil if it was not found): the engine's function itself,
    -- called through Gear's replacement.
    function auto.engine_free_slot() return native.engine_free_slot() end
    -- Tests the stubs of the constant site hooks on two functions built in memory; a worker thread calls them when Lua is idle, so
    -- the handlers of `combat.constant` may answer for `selftest.fmul` / `selftest.fld`.
    function auto.site_hook_selftest_start() return native.site_selftest_start() end
    function auto.site_hook_selftest_result()
        local fmul, fld, regs, store, add, edx, kept, damage, exit_, e0, e1, counter, bonus_int, bonus_float, slot_byte, branch_taken, branch2_path, branch2_eax = native.site_selftest_result()
        if fmul == nil then return nil end
        return {fmul = fmul, fld = fld, registers_kept = regs, ability_store = store, ability_add = add, ability_edx = edx, ability_kept = kept, damage = damage,
                effect_exit = exit_, effect_entry0 = e0, effect_entry1 = e1, effect_counter = counter, bonus_int = bonus_int, bonus_float = bonus_float, slot_byte = slot_byte, branch_taken = branch_taken,
                branch2_path = branch2_path, branch2_eax = branch2_eax}
    end
    function auto.start_gameserver(seconds)
        return native.auto_start_gameserver(seconds or 8)
    end
    -- auto.show_window(name, shown) -> bool: shows or hides an in-game window by its control name (UI_WND_CHEST ...)
    -- through the window's own show, without the events that normally open it. For self tests.
    function auto.show_window(name, shown)
        assert(type(name) == 'string', 'name must be a string')
        return native.auto_show_window(name, shown and true or false)
    end
    -- auto.network_flag(on) -> bool: sets or clears the engine's network-game flag, to test that the guards (can_change,
    -- inventory and spawn) refuse in a network game. Set it, test, clear it in the same tick.
    -- auto.network_mode(mode) -> bool: sets the flags gear.mp.mode() reads ('single', 'lan', 'open', 'closed'), for self tests
    -- of what depends on the kind of game. Set it, test, put the old mode back in the same tick.
    function auto.network_mode(mode)
        assert(mode == 'single' or mode == 'lan' or mode == 'open' or mode == 'closed' or mode == 'internet',
            "mode must be 'single', 'lan', 'open', 'closed' or 'internet'")
        return native.auto_network_mode(mode)
    end
    -- auto.hero_from_server(on): the game server delivered the hero (message 0x132) or not; 'open' reads as closed while it is set
    function auto.hero_from_server(on) native.auto_hero_from_server(on and true or false) end
    -- auto.chat_add(text, channel, color) -> bool: a line on the game's message list, as the game adds one (gear.chat self tests).
    function auto.chat_add(text, channel, color)
        assert(type(text) == 'string', 'text must be a string')
        return native.auto_chat_add(text, channel or 1, color or 0)
    end
    -- auto.chat_send(kind, target, text) -> bool: a chat message on the network ('private' to slot target, 'team', 'global').
    function auto.chat_send(kind, target, text)
        local t = ({private = 3, team = 4, global = 5})[kind]
        assert(t, "kind must be 'private', 'team' or 'global'")
        assert(type(text) == 'string', 'text must be a string')
        return native.auto_chat_send(t, target or 0, text)
    end
    function auto.network_flag(on)
        return native.auto_network_flag(on and true or false)
    end
    function auto.quit(code)
        native.auto_release()
        native.auto_quit(code or 0)
    end
    -- In a task: until the main menu is up (true) or `timeout` passed (false).
    function auto.wait_for_menu(timeout)
        return wait_until(function()
            local s = native.auto_state()
            return s.menus and not s.in_game and not s.dialog and not s.cinematic
        end, timeout or 120)
    end
    -- In a task: until a game runs with a hero (true) or `timeout` passed (false).
    function auto.wait_for_game(timeout)
        return wait_until(function() return native.hero() ~= nil end, timeout or 180)
    end
    return auto
end

---------------------------------------------------------------------------------------------------------------
-- The mod manager's data. The catalog lists every installed mod (loaded or not); mods\gear_mods.json keeps what the
-- player chose: enabled flags, setting values and key binds, for all mods in one file. Mods declare settings and
-- keys in mod.lua; the screen that shows them is modmanager.lua (a mod of its own, built into the DLL).
---------------------------------------------------------------------------------------------------------------
local catalog = {}          -- mod id -> entry {id, dir, m, builtin, invalid, loaded, want, reason, fail, excluded}
local catalog_list = {}     -- the entries by id
-- The theme mod: the first running mod (in the catalog's order) whose mod.lua names a `theme` stylesheet. Returns its folder and the file.
function uis.theme_provider()
    for _, e in ipairs(catalog_list) do
        if e.loaded and e.m and e.m.theme then return e.dir, e.m.theme end
    end
end
local mod_state = nil       -- gear_mods.json decoded: {mods = {id = {enabled, settings, keys, migrated}}}
local state_dirty = false
local key_handlers = {}     -- array of {mod, id, def, fn, entry, was}
local keys_blocked = false  -- the mod manager's screen is open: other mods' hotkeys do not fire
local apply_pending = false -- a mod was switched on or off: kernel.tick loads and unloads before it runs the handlers
local admin = {}            -- what only the mod manager sees (filled in below the loader)

local function is_table(v) return type(v) == 'table' and v ~= json.null end

local function state_load()
    if mod_state then return mod_state end
    mod_state = {mods = {}}
    local text = native.state_read()
    if text then
        local v, err = json.decode(text)
        if is_table(v) and (v.mods == nil or is_table(v.mods)) then
            for id, e in pairs(v.mods or {}) do
                if is_table(e) then mod_state.mods[id] = e end
            end
        else
            log('gear_mods.json cannot be read (%s): it is kept as gear_mods.json.bak, the defaults apply',
                tostring(err or 'not an object'))
            native.state_write(text, true)
        end
    end
    return mod_state
end

-- The saved record of a mod (nil unless `create`).
local function state_of(id, create)
    local all = state_load().mods
    local e = all[id]
    if not e and create then e = {}; all[id] = e end
    return e
end

-- A table of that record: 'settings', 'keys' or 'migrated' (nil unless `create`).
local function state_sub(id, name, create)
    local e = state_of(id, create)
    if not e then return nil end
    if not is_table(e[name]) then
        if not create then return nil end
        e[name] = {}
    end
    return e[name]
end

local function state_flush()
    if not state_dirty then return end
    state_dirty = false
    local out = {}
    for id, e in pairs(state_load().mods) do
        local c = {}
        if type(e.enabled) == 'boolean' then c.enabled = e.enabled end
        for _, name in ipairs({'settings', 'keys', 'migrated'}) do
            if is_table(e[name]) and next(e[name]) ~= nil then c[name] = e[name] end
        end
        if next(c) ~= nil then out[id] = c end
    end
    if not native.state_write(json.encode({version = 1, mods = out})) then
        log('gear_mods.json: the file cannot be written')
    end
end

-- Realms ----------------------------------------------------------------------------------------------------------
-- The online play (register / login) server. The choice itself is native (native.realm_select: the game's network menu
-- takes the host and the lobby port from it when Closed or Open Internet opens, see gear/realm.h); this keeps the
-- list and the active realm in mods\gear_realms.json. The built-in realm 'default' is what Settings.cfg says.
local realm_api, realm_poll, realm_flush   -- (the rest is local to the block below: the chunk's 200 locals)
do
    local store = nil          -- {default_host, realms = {{id, name, host, port?}}, active}
    local synced = false       -- the saved choice was handed to the native side
    local dirty = false
    local ID_PATTERN = '^[%w_%-]+$'

    local function load_store()
        if store then return store end
        store = {realms = {}, active = 'default'}
        local text = native.state_read('realms')
        if text then
            local v, err = json.decode(text)
            if is_table(v) then
                if type(v.default_host) == 'string' and native.realm_valid_host(v.default_host) then
                    store.default_host = v.default_host
                end
                for _, r in ipairs(is_table(v.realms) and v.realms or {}) do
                    if is_table(r) and type(r.id) == 'string' and r.id:match(ID_PATTERN) and r.id ~= 'default'
                        and type(r.host) == 'string' and native.realm_valid_host(r.host) then
                        store.realms[#store.realms + 1] = {
                            id = r.id, name = type(r.name) == 'string' and r.name or r.id, host = r.host,
                            port = type(r.port) == 'number' and r.port or nil,
                        }
                    end
                end
                if type(v.active) == 'string' then store.active = v.active end
            else
                log('gear_realms.json cannot be read (%s): it is kept as gear_realms.json.bak, the defaults apply',
                    tostring(err or 'not an object'))
                native.state_write(text, true, 'realms')
            end
        end
        return store
    end

    function realm_flush()
        if not dirty then return end
        dirty = false
        local st = load_store()
        local out = {}
        for _, r in ipairs(st.realms) do out[#out + 1] = {id = r.id, name = r.name, host = r.host, port = r.port} end
        if not native.state_write(json.encode({version = 1, default_host = st.default_host, realms = out, active = st.active}),
            false, 'realms') then
            log('gear_realms.json: the file cannot be written')
        end
    end

    local function copy(r)
        return {id = r.id, name = r.name, host = r.host, port = r.port, builtin = r.id == 'default' or nil}
    end

    local function default_realm()
        return {id = 'default', name = 'Default (Settings.cfg)', host = load_store().default_host or ''}
    end

    local function find(id)
        if id == 'default' then return default_realm() end
        for _, r in ipairs(load_store().realms) do
            if r.id == id then return r end
        end
    end

    -- Hands the saved choice to the native side (once) and remembers the game's own host when it shows (the network
    -- object exists after the first network menu; until then the default realm has no host to show).
    function realm_poll()
        local st = load_store()
        if not synced then
            synced = true
            local r = find(st.active)
            if not r then
                st.active = 'default'
                dirty = true
                native.realm_select(nil)
            elseif st.active == 'default' then
                native.realm_select(nil)
            else
                native.realm_select(r.host, r.port)
            end
        end
        if not st.default_host then
            local now = native.realm_get()
            if now and now.game_host and now.game_host ~= '' then
                st.default_host = now.game_host
                dirty = true
            end
        end
    end

    realm_api = {
        -- list() -> Realm[]: the built-in 'default' first, then the added ones.
        list = function()
            realm_poll()
            local out = {copy(default_realm())}
            for _, r in ipairs(load_store().realms) do out[#out + 1] = copy(r) end
            return out
        end,
        -- current() -> Realm: the chosen realm.
        current = function()
            realm_poll()
            return copy(find(load_store().active) or default_realm())
        end,
        -- add(def) -> true | nil, message: def {id, name?, host, port?}; an existing id is replaced.
        add = function(def)
            assert(type(def) == 'table', 'add wants a table {id, name, host, port?}')
            local id, host = def.id, def.host
            if type(id) ~= 'string' or not id:match(ID_PATTERN) or #id > 32 then
                return nil, 'id: 1 to 32 letters, digits, "_" or "-"'
            end
            if id == 'default' then return nil, 'the id "default" is the built-in realm' end
            if type(host) ~= 'string' or not native.realm_valid_host(host) then return nil, 'host: not a valid host name' end
            local port = def.port
            if port ~= nil and (type(port) ~= 'number' or port ~= math.floor(port) or port < 1 or port > 65535) then
                return nil, 'port: a number from 1 to 65535'
            end
            local entry = {id = id, name = type(def.name) == 'string' and def.name ~= '' and def.name or id, host = host, port = port}
            local st = load_store()
            for i, r in ipairs(st.realms) do
                if r.id == id then
                    st.realms[i] = entry
                    dirty = true
                    if st.active == id then native.realm_select(host, port) end
                    return true
                end
            end
            st.realms[#st.realms + 1] = entry
            dirty = true
            return true
        end,
        -- remove(id) -> true | nil, message: the active realm cannot be removed (switch first).
        remove = function(id)
            assert(type(id) == 'string', 'remove wants a realm id')
            local st = load_store()
            if id == 'default' then return nil, 'the built-in realm cannot be removed' end
            if st.active == id then return nil, 'this realm is in use: switch to another first' end
            for i, r in ipairs(st.realms) do
                if r.id == id then table.remove(st.realms, i); dirty = true; return true end
            end
            return nil, 'no such realm'
        end,
        -- use(id) -> true | nil, message: the next Closed or Open Internet the player opens in the multiplayer menu uses
        -- this realm's host (and port). Raises 'realm.changed' when the realm changed.
        use = function(id)
            assert(type(id) == 'string', 'use wants a realm id')
            realm_poll()
            local r = find(id)
            if not r then return nil, 'no such realm' end
            local ok, err
            if id == 'default' then ok, err = native.realm_select(nil) else ok, err = native.realm_select(r.host, r.port) end
            if not ok then return nil, err end
            local st = load_store()
            local changed = st.active ~= id
            st.active = id
            dirty = true
            log('Realm: %s (%s)%s', id, r.host, current and ' by ' .. current.id or '')
            if changed then kernel.dispatch('realm.changed', copy(r)) end
            return true
        end,
    }
end

-- Settings ------------------------------------------------------------------------------------------------------
local SETTING_TYPES = {bool = true, number = true, choice = true, text = true, color = true}

-- A color as the number 0xAARRGGBB from a number, {r, g, b, a} or '#AARRGGBB' / '#RRGGBB'.
local function parse_color(v)
    if type(v) == 'number' and v == v then return math.floor(v) % 4294967296 end
    if type(v) == 'table' then
        local ok, c = pcall(ui_color, v)
        return ok and c or nil
    end
    if type(v) == 'string' then
        local a, rgb = v:match('^#?(%x%x)(%x%x%x%x%x%x)$')
        if a then return tonumber(a .. rgb, 16) end
        rgb = v:match('^#?(%x%x%x%x%x%x)$')
        if rgb then return tonumber('FF' .. rgb, 16) end
    end
    return nil
end

-- `v` made to fit the setting's type (clamped, on the step's grid, one of the choices), or nil if it cannot be.
local function coerce(def, v)
    local t = def.type
    if t == 'bool' then
        if type(v) == 'boolean' then return v end
    elseif t == 'number' then
        if type(v) ~= 'number' or v ~= v then return nil end
        v = math.max(def.min, math.min(def.max, v))
        if def.step then
            v = def.min + math.floor((v - def.min) / def.step + 0.5) * def.step
            v = math.max(def.min, math.min(def.max, v))
        end
        return tonumber(string.format('%.10g', v))
    elseif t == 'choice' then
        for _, c in ipairs(def.choices) do
            if c.value == v then return v end
        end
    elseif t == 'text' then
        if type(v) == 'string' and #v <= def.max_length then return v end
    elseif t == 'color' then
        return parse_color(v)
    end
    return nil
end

-- The declared settings of a manifest: the list of normalized definitions and the same by id, or nil and a message.
local function parse_settings(list)
    if list == nil then return {}, {} end
    if type(list) ~= 'table' then return nil, 'mod.lua: settings must be a list' end
    local defs, by_id = {}, {}
    for i, s in ipairs(list) do
        if type(s) ~= 'table' then return nil, 'mod.lua: settings[' .. i .. '] must be a table' end
        if type(s.id) ~= 'string' or not s.id:match('^[%w_%.%-]+$') then
            return nil, 'mod.lua: settings[' .. i .. ']: id must be letters, digits, _ - and .'
        end
        if by_id[s.id] then return nil, 'mod.lua: settings: two settings with the id ' .. s.id end
        if not SETTING_TYPES[s.type] then
            return nil, 'mod.lua: setting ' .. s.id .. ': type must be bool, number, choice, text or color'
        end
        local d = {id = s.id, type = s.type, label = type(s.label) == 'string' and s.label or s.id,
                   section = type(s.section) == 'string' and s.section or nil,
                   tooltip = type(s.tooltip) == 'string' and s.tooltip or nil, restart = s.restart == true,
                   note = type(s.note) == 'string' and s.note or nil}
        local fallback
        if d.type == 'number' then
            d.min = type(s.min) == 'number' and s.min or 0
            d.max = type(s.max) == 'number' and s.max or 100
            if d.max < d.min then return nil, 'mod.lua: setting ' .. s.id .. ': max is below min' end
            d.step = (type(s.step) == 'number' and s.step > 0) and s.step or 1
            fallback = d.min
        elseif d.type == 'choice' then
            d.choices = {}
            if type(s.choices) ~= 'table' or #s.choices == 0 then
                return nil, 'mod.lua: setting ' .. s.id .. ': choices must be a list'
            end
            for _, c in ipairs(s.choices) do
                local value, label = c, c
                if type(c) == 'table' then value, label = c.value, c.label or c.value end
                if type(value) ~= 'string' and type(value) ~= 'number' then
                    return nil, 'mod.lua: setting ' .. s.id .. ': a choice is a string, a number or {value, label}'
                end
                d.choices[#d.choices + 1] = {value = value, label = tostring(label)}
            end
            fallback = d.choices[1].value
        elseif d.type == 'text' then
            d.max_length = (type(s.max_length) == 'number' and s.max_length >= 1) and math.floor(s.max_length) or 120
            fallback = ''
        elseif d.type == 'color' then
            fallback = 0xFFFFFFFF
        else
            fallback = false
        end
        local dv = coerce(d, s.default)
        if dv == nil then dv = fallback end
        d.default = dv
        defs[#defs + 1] = d
        by_id[d.id] = d
    end
    return defs, by_id
end

-- Key binds ------------------------------------------------------------------------------------------------------
local MOD_ORDER = {'ctrl', 'shift', 'alt'}
local MOD_ALIAS = {ctrl = 'ctrl', control = 'ctrl', shift = 'shift', alt = 'alt'}

-- 'ctrl+shift+m' -> {ctrl, shift, alt, key, vk}, or nil.
local function parse_binding(text)
    if type(text) ~= 'string' then return nil end
    local parts = {}
    for p in text:lower():gmatch('[^%+]+') do parts[#parts + 1] = (p:gsub('^%s+', ''):gsub('%s+$', '')) end
    if #parts == 0 then return nil end
    local key = parts[#parts]
    local vk = UI_VK[key]
    -- A modifier alone is a binding too (Alt shows the names, Ctrl stands still): only for what the game polls.
    if not vk or (MOD_ALIAS[key] and #parts > 1) or key == 'escape' then return nil end
    local b = {key = key, vk = vk}
    for i = 1, #parts - 1 do
        local m = MOD_ALIAS[parts[i]]
        if not m then return nil end
        b[m] = true
    end
    return b
end

-- The canonical text of a binding ('Ctrl+M' -> 'ctrl+m'; '' unbound), or nil if it is not one.
local function normalize_binding(text)
    if text == '' then return '' end
    local b = parse_binding(text)
    if not b then return nil end
    local out = {}
    for _, m in ipairs(MOD_ORDER) do
        if b[m] then out[#out + 1] = m end
    end
    out[#out + 1] = b.key
    return table.concat(out, '+')
end

local function parse_keys(list)
    if list == nil then return {}, {} end
    if type(list) ~= 'table' then return nil, 'mod.lua: keys must be a list' end
    local defs, by_id = {}, {}
    for i, k in ipairs(list) do
        if type(k) ~= 'table' then return nil, 'mod.lua: keys[' .. i .. '] must be a table' end
        if type(k.id) ~= 'string' or not k.id:match('^[%w_%.%-]+$') then
            return nil, 'mod.lua: keys[' .. i .. ']: id must be letters, digits, _ - and .'
        end
        if by_id[k.id] then return nil, 'mod.lua: keys: two keys with the id ' .. k.id end
        local default = normalize_binding(k.default == nil and '' or k.default)
        if not default then
            return nil, 'mod.lua: key ' .. k.id .. ': "' .. tostring(k.default) .. '" is not a key like ctrl+m or f8'
        end
        local d = {id = k.id, label = type(k.label) == 'string' and k.label or k.id, default = default,
            section = type(k.section) == 'string' and k.section or nil, second = type(k.second) == 'string' and k.second or nil}
        defs[#defs + 1] = d
        by_id[d.id] = d
    end
    -- `second`: the id of another key that is this one's second slot; the screen shows both on one row.
    for _, d in ipairs(defs) do
        if d.second then
            local o = by_id[d.second]
            if not o or o == d or o.second then return nil, 'mod.lua: key ' .. d.id .. ': second must be another key without a second' end
            o.is_second = true
        end
    end
    return defs, by_id
end

-- Whether the mod is switched on in the manager: the player's flag, else mod.lua's `enabled` (false if -gear-mods
-- left it out).
local function desired(e)
    local st = state_of(e.id, false)
    if st and type(st.enabled) == 'boolean' then return st.enabled end
    return e.default_enabled
end

-- Which mods run: switched on, and every mod they require runs too. Sets entry.want and entry.reason.
local function compute_want()
    local memo = {}
    local function want(e)
        if e.builtin then return true end
        if memo[e] ~= nil then return memo[e] end
        memo[e] = false
        e.reason = nil
        if e.invalid or not desired(e) then return false end
        for _, dep in ipairs(e.m.requires) do
            local d = catalog[dep]
            if not d or d.invalid then
                e.reason = 'requires ' .. tostring(dep) .. ', which is not installed'
                return false
            end
            if not want(d) then
                e.reason = 'requires ' .. dep .. ', which is off'
                return false
            end
        end
        memo[e] = true
        return true
    end
    for _, e in ipairs(catalog_list) do e.want = want(e) end
end

local function setting_default(e, def)
    local c = e.m.config[def.id]
    if c ~= nil then
        local v = coerce(def, c)
        if v ~= nil then return v end
    end
    return def.default
end

local function setting_value(e, def)
    local saved = state_sub(e.id, 'settings', false)
    local sv = saved and saved[def.id]
    if sv ~= nil then
        local v = coerce(def, sv)
        if v ~= nil then return v end
    end
    return setting_default(e, def)
end

local function settings_set(e, def, value)
    local v = coerce(def, value)
    if v == nil then return nil, 'not a valid value for ' .. def.id end
    local old = setting_value(e, def)
    state_sub(e.id, 'settings', true)[def.id] = v
    state_dirty = true
    if v ~= old and e.loaded then kernel.dispatch_to(e.m, 'settings.changed', def.id, v) end
    return true
end

-- Forgets the saved value of one setting (or of all): the default applies again.
local function settings_reset(e, def)
    local saved = state_sub(e.id, 'settings', false)
    if not saved then return end
    for _, d in ipairs(e.m.settings) do
        if (def == nil or def == d) and saved[d.id] ~= nil then
            local old = setting_value(e, d)
            saved[d.id] = nil
            state_dirty = true
            local v = setting_value(e, d)
            if v ~= old and e.loaded then kernel.dispatch_to(e.m, 'settings.changed', d.id, v) end
        end
    end
end

local function key_binding(e, def)
    local saved = state_sub(e.id, 'keys', false)
    local s = saved and saved[def.id]
    if type(s) == 'string' then
        local n = normalize_binding(s)
        if n then return n end
    end
    return def.default
end

-- text nil: back to the default; '' unbound.
local function key_set(e, def, text)
    if text ~= nil then
        local n = normalize_binding(text)
        if not n then return nil, 'not a key like ctrl+m or f8' end
        state_sub(e.id, 'keys', true)[def.id] = n
    else
        local saved = state_sub(e.id, 'keys', false)
        if saved then saved[def.id] = nil end
    end
    state_dirty = true
    return true
end

-- The game's own hotkeys (SacredBild's bindings table: the keyboard defaults of the game), by key name. The game
-- reacts to these keys whatever modifiers are held, so a mod bind on one of them clashes with the game.
local GAME_KEYS = {
    ['1'] = 'Weapon slot 1', ['2'] = 'Weapon slot 2', ['3'] = 'Weapon slot 3', ['4'] = 'Weapon slot 4',
    ['5'] = 'Weapon slot 5', ['6'] = 'Combat art 1', ['7'] = 'Combat art 2', ['8'] = 'Combat art 3',
    ['9'] = 'Combat art 4', ['0'] = 'Combat art 5', space = 'Healing potion', q = 'Potion of Undead Death',
    w = 'Potion of the Mentor', e = "Viper's Antidote", r = 'Potion of Concentration', b = 'Heal hirelings',
    a = 'Collect all visible objects', i = 'Inventory', f = 'Combat arts', c = 'Combo menu', l = 'Log book',
    m = 'World map', tab = 'Overview map', o = 'Options', s = 'Save menu', h = 'Help screen', n = 'Party information',
    f9 = 'Quick save', f8 = 'Quick load', p = 'Pause', add = 'Zoom in', subtract = 'Zoom out', escape = 'Game menu',
}

-- The other keys (of mods that run or are switched on) with the same binding: {{mod, name, key, label}}; the game's
-- own hotkeys too, as {mod = '', name = 'The game', key = '', label = <what it does>, game = true}.
local function key_conflicts(e, def)
    local out = {}
    local mine = key_binding(e, def)
    if mine == '' then return out end
    local keyname = mine:match('[^%+]+$')
    local game = GAME_KEYS[keyname]
    -- A key a mod took over with gear.input (the game's action moved elsewhere, or the mod handles it) is no longer the game's.
    if game and not uis.input.owned[keyname] then out[#out + 1] = {mod = '', name = 'The game', key = '', label = game, game = true} end
    for _, o in ipairs(catalog_list) do
        if o.want ~= false and not o.invalid then
            for _, k in ipairs(o.m.keys) do
                if not (o == e and k == def) and key_binding(o, k) == mine then
                    out[#out + 1] = {mod = o.id, name = o.m.name, key = k.id, label = k.label}
                end
            end
        end
    end
    return out
end

local parsed_bindings = {}
local function binding_down(b)
    if not native.key_down(b.vk) then return false end
    for _, m in ipairs(MOD_ORDER) do
        if (native.key_down(UI_VK[m]) == true) ~= (b[m] == true) then return false end
    end
    return true
end

-- The declared keys' handlers: fired once per press, with exactly the bound modifiers held.
local function poll_keys()
    if #key_handlers == 0 then return end
    for _, h in ipairs({unpack(key_handlers)}) do
        if not h.mod.disabled then
            local text = key_binding(h.entry, h.def)
            local down = false
            if text ~= '' then
                local b = parsed_bindings[text]
                if b == nil then b = parse_binding(text) or false; parsed_bindings[text] = b end
                down = b and binding_down(b) or false
            end
            local fire = down and not h.was
            h.was = down
            if fire and (not keys_blocked or h.mod.builtin) then
                call(h.mod, 'key ' .. h.def.id, h.fn)
            end
        end
    end
end

-- What the mod manager and gear.mods tell about a mod.
local function mod_info(e)
    local m = e.m
    local state
    if e.invalid then state = 'invalid'
    elseif e.loaded and e.want == false then state = desired(e) and 'blocked' or 'off'     -- going down next tick
    elseif e.loaded and m.disabled then state = 'error'
    elseif e.loaded then state = 'on'
    elseif e.fail then state = 'error'
    elseif desired(e) and not e.want then state = 'blocked'
    elseif desired(e) then state = 'pending'
    else state = 'off' end
    local requires = {}
    for i, r in ipairs(m.requires or {}) do requires[i] = r end
    return {id = e.id, name = m.name, version = m.version or '', author = m.author or '', description = m.description or '',
        state = state, enabled = e.builtin or desired(e), loaded = e.loaded == true, builtin = e.builtin == true,
        reason = e.fail or e.reason or e.problem, errors = e.loaded and m.errors or 0, requires = requires,
        has_settings = #m.settings > 0, has_keys = #m.keys > 0, has_page = e.loaded == true and m.page ~= nil,
        dir = e.dir}
end

local function make_settings_api(mod)
    local api = {}
    local function def_of(id)
        local d = mod.settings_by_id[id]
        if not d then error('this mod declares no setting "' .. tostring(id) .. '"', 3) end
        return d
    end
    -- game(key) -> string?: a value of the game's Settings.cfg ("KEY : value" lines), as text.
    function api.game(key)
        assert(type(key) == 'string', 'key must be a string')
        return native.setting(key)
    end
    -- get(id): the value of a setting this mod declares; an undeclared UPPER_CASE name is a Settings.cfg key and gives that text (nil if absent).
    -- get(id, fallback): with a fallback, an undeclared id gives the fallback instead of an error, and so does a nil value.
    function api.get(id, fallback)
        local d = mod.settings_by_id[id]
        if not d and type(id) == 'string' and id:match('^[A-Z0-9_]+$') then
            local v = native.setting(id)
            if v == nil then return fallback end
            return v
        end
        if not d and fallback ~= nil then return fallback end
        local v = setting_value(mod.entry, d or def_of(id))
        if v == nil then return fallback end
        return v
    end
    function api.set(id, value)
        return settings_set(mod.entry, def_of(id), value)
    end
    -- Whether the player has set this one (else its value is the default).
    function api.is_set(id)
        local d = def_of(id)
        local saved = state_sub(mod.id, 'settings', false)
        return saved ~= nil and saved[d.id] ~= nil
    end
    function api.all()
        local out = {}
        for _, d in ipairs(mod.settings) do out[d.id] = setting_value(mod.entry, d) end
        return out
    end
    function api.reset(id)
        settings_reset(mod.entry, id ~= nil and def_of(id) or nil)
    end
    -- Once per mod: reads the old settings file `name` of the mod's folder (JSON) and hands the decoded table to `fn`,
    -- which copies what it wants into gear.settings.
    function api.migrate(name, fn)
        assert(type(name) == 'string' and type(fn) == 'function', 'migrate(name, fn)')
        local done = state_sub(mod.id, 'migrated', false)
        if done and done[name] then return false end
        local text = native.read_file(mod.dir, name)
        if not text then return false end
        local v, err = json.decode(text)
        if is_table(v) then
            fn(v)
        else
            log('[%s] %s cannot be migrated: %s', mod.id, name, tostring(err))
        end
        state_sub(mod.id, 'migrated', true)[name] = true
        state_dirty = true
        return true
    end
    return api
end

local function make_keys_api(mod)
    local api = {}
    local function def_of(id)
        local d = mod.keys_by_id[id]
        if not d then error('this mod declares no key "' .. tostring(id) .. '"', 3) end
        return d
    end
    function api.on(id, fn)
        assert(type(fn) == 'function', 'handler must be a function')
        local h = {mod = mod, id = id, def = def_of(id), fn = fn, entry = mod.entry}
        key_handlers[#key_handlers + 1] = h
        return h
    end
    function api.off(h)
        for i = #key_handlers, 1, -1 do
            if key_handlers[i] == h then table.remove(key_handlers, i) end
        end
    end
    function api.binding(id)
        return key_binding(mod.entry, def_of(id))
    end
    function api.set(id, binding)
        return key_set(mod.entry, def_of(id), binding)
    end
    function api.label(id)
        local b = key_binding(mod.entry, def_of(id))
        if b == '' then return '' end
        local out = {}
        for part in b:gmatch('[^%+]+') do out[#out + 1] = part:sub(1, 1):upper() .. part:sub(2) end
        return table.concat(out, '+')
    end
    return api
end

-- gear.input: the game's own keys, remapped (gear/input.cpp). State lives in `uis` (the main chunk is at its locals limit).
uis.input = {rules = {}, handlers = {}, dirty = false, owned = {},
    -- keys the game reads by polling, not through key messages
    POLLED = {ctrl = 0x11, control = 0x11, shift = 0x10, alt = 0x12, add = 0x6B, subtract = 0x6D}}
function uis.make_input(mod)
    local api = {}
    local function vk_of(name, what)
        local vk = UI_VK[tostring(name):lower()]
        if not vk or vk == 0x1B or MOD_ALIAS[tostring(name):lower()] then
            error(what .. ' is not a key or mouse button: ' .. tostring(name), 3)
        end
        return vk
    end
    -- gear.input.remap(rules): this mod's rules (replaces the earlier ones; {} removes them). A rule:
    --   {from = 'shift+mouse4', to = '6', id = 'art1'}   the input (a key or mouse 3 to 5, with exactly the modifiers named)
    --                                                    is the game's key `to`, as if the player pressed that
    --   {from = '6', block = true}                       the input does nothing in game (any modifiers)
    -- Only while the game has the keyboard (in game, nobody typing, no message box or escape menu).
    function api.remap(rules)
        assert(type(rules) == 'table', 'rules must be a list')
        local list = {}
        for i, r in ipairs(rules) do
            assert(type(r) == 'table', 'rule ' .. i .. ' must be a table')
            if r.release then
                -- {release = 'alt'}: the game's own poll of that key sees it released
                local vk = uis.input.POLLED[tostring(r.release):lower()]
                if not vk then error('rule ' .. i .. ': release must be ctrl, shift, alt, add or subtract', 2) end
                list[#list + 1] = {from = vk, poll = true, any = true, id = ''}
                goto continue
            end
            local b = parse_binding(r.from)
            if not b then error('rule ' .. i .. ': from is not a key: ' .. tostring(r.from), 2) end
            local entry = {from = b.vk, ctrl = b.ctrl, shift = b.shift, alt = b.alt, id = r.id and (mod.id .. ':' .. r.id) or ''}
            if r.block then
                entry.any = true
            else
                local to = tostring(r.to):lower()
                if uis.input.POLLED[to] then
                    -- a key the game polls: its poll sees the input (the input's messages go nowhere)
                    entry.to, entry.poll = uis.input.POLLED[to], true
                else
                    entry.to = vk_of(r.to, 'rule ' .. i .. ': to')
                end
            end
            entry.owns = not (b.ctrl or b.shift or b.alt) and b.key or nil
            -- "this key is that same key" changes nothing, unless events are wanted: it only keeps the key from the game's
            -- other uses for the mod (see the conflicts)
            if not (entry.to == entry.from and not (entry.ctrl or entry.shift or entry.alt) and entry.id == '') then
                list[#list + 1] = entry
            elseif entry.owns then
                list.owned = list.owned or {}
                list.owned[entry.owns] = true
            end
            ::continue::
        end
        uis.input.rules[mod] = (#list > 0 or list.owned) and list or nil
        uis.input.dirty = true
    end
    -- gear.input.on(fn): fn(id, down, repeat) when the input of one of this mod's rules with an id is pressed or
    -- released (`repeat`: the key's auto repeat).
    function api.on(fn)
        assert(type(fn) == 'function', 'handler must be a function')
        local h = {mod = mod, fn = fn}
        uis.input.handlers[#uis.input.handlers + 1] = h
        return h
    end
    function api.off(h)
        for i = #uis.input.handlers, 1, -1 do
            if uis.input.handlers[i] == h then table.remove(uis.input.handlers, i) end
        end
    end
    -- gear.input.context() -> ok, typing: whether the layer applies now (in game, nobody typing), and whether keys go to text.
    function api.context()
        if not native.input_context then return false, false end
        return native.input_context()
    end
    -- Rebinding: capture_start(), then capture() each frame returns the binding ('ctrl+mouse4') of the first key or
    -- mouse button pressed since, or nil. A screen that does this calls gear.ui.capture_keys(true) as well.
    function api.capture_start() admin.capture_start() end
    function api.capture() return admin.capture() end
    -- gear.input.normalize(text) -> canonical binding text, or nil if it is not one ('' stays '').
    function api.normalize(text) return normalize_binding(text) end
    return api
end

-- Publishes the rules of every mod that runs, and hands the events to the mods' handlers.
function uis.input_poll()
    local I = uis.input
    if I.dirty then
        I.dirty = false
        local all = {}
        I.owned = {}
        for m, list in pairs(I.rules) do
            if m.disabled then
                I.rules[m] = nil
            else
                for _, r in ipairs(list) do
                    all[#all + 1] = r
                    if r.owns then I.owned[r.owns] = true end
                end
                for name in pairs(list.owned or {}) do I.owned[name] = true end
            end
        end
        -- A rule with an exact binding goes before a blocking one of the same key (the native code also prefers it).
        if native.input_rules then native.input_rules(all) end
    end
    if native.input_events and next(I.rules) then
        for _, ev in ipairs(native.input_events()) do
            local mid, id = ev.id:match('^([^:]*):(.*)$')
            for _, h in ipairs({unpack(I.handlers)}) do
                if h.mod.id == mid and not h.mod.disabled then call(h.mod, 'input ' .. id, h.fn, id, ev.down, ev['repeat']) end
            end
        end
    end
end

-- gear.menu: menus the mods can add items to, and the mod that draws them (a "native menu", for instance one built on gear.ui.rml). Items are
-- kept here whether or not a native menu exists, so a mod registers its entry once; it draws its own button only while
-- gear.menu.available(menu) is false.
uis.menu = {hosts = {}, items = {}}
function uis.make_menu(mod)
    local api = {}
    local state = uis.menu
    local function alive(m) return m ~= nil and not m.disabled and not m.unloaded end
    -- menu.host(menu, on) -> true | false, message: this mod draws `menu` ('esc'): the game menu).
    function api.host(menu, on)
        assert(type(menu) == 'string', 'menu must be a string')
        local owner = state.hosts[menu]
        if on == false then
            if owner == mod then state.hosts[menu] = nil end
            return true
        end
        if alive(owner) and owner ~= mod then return false, menu .. ' is drawn by ' .. tostring(owner.id) end
        state.hosts[menu] = mod
        return true
    end
    -- menu.available(menu) -> whether a mod draws the menu now (default 'esc').
    function api.available(menu) return alive(state.hosts[menu or 'esc']) end
    -- menu.add_item(menu, {id, label, on_click, order, tooltip, enabled}): the same id of this mod replaces its item.
    function api.add_item(menu, spec)
        assert(type(menu) == 'string', 'menu must be a string')
        assert(type(spec) == 'table' and type(spec.id) == 'string' and spec.id ~= '', 'spec.id must be a string')
        assert(type(spec.on_click) == 'function', 'spec.on_click must be a function')
        assert(type(spec.label) == 'string' or type(spec.label) == 'function', 'spec.label must be a string or a function')
        local list = state.items[menu] or {}
        state.items[menu] = list
        local item = {mod = mod, id = spec.id, label = spec.label, on_click = spec.on_click, order = tonumber(spec.order) or 100,
            tooltip = spec.tooltip, enabled = spec.enabled}
        for i, it in ipairs(list) do
            if it.mod == mod and it.id == spec.id then list[i] = item; return true end
        end
        list[#list + 1] = item
        return true
    end
    function api.remove_item(menu, id)
        local list = state.items[menu] or {}
        for i = #list, 1, -1 do
            if list[i].mod == mod and list[i].id == id then table.remove(list, i); return true end
        end
        return false
    end
    -- menu.items(menu) -> {{id, mod, label, tooltip, enabled, order, click}, ...} by order, of the mods that are on; `click()` runs the item's function.
    function api.items(menu)
        local out = {}
        for _, it in ipairs(state.items[menu] or {}) do
            local m = it.mod
            if alive(m) then
                local label = it.label
                if type(label) == 'function' then
                    local ok, text = pcall(label)
                    label = ok and tostring(text) or '?'
                end
                local enabled = it.enabled
                if type(enabled) == 'function' then enabled = select(2, pcall(enabled)) end
                out[#out + 1] = {id = it.id, mod = m.id, label = label, tooltip = it.tooltip, enabled = enabled ~= false, order = it.order,
                    click = function() call(m, 'menu item ' .. it.id, it.on_click) end}
            end
        end
        table.sort(out, function(a, b)
            if a.order ~= b.order then return a.order < b.order end
            return a.label < b.label
        end)
        return out
    end
    return api
end
function uis.drop_menu(m)
    for menu, owner in pairs(uis.menu.hosts) do
        if owner == m then uis.menu.hosts[menu] = nil end
    end
    for _, list in pairs(uis.menu.items) do
        for i = #list, 1, -1 do
            if list[i].mod == m then table.remove(list, i) end
        end
    end
end

local function make_mods_api(mod)
    local api = {}
    -- A page of this mod's own in its settings screen (after the generated settings): fn(x, y, w, h) draws in that
    -- rectangle with this mod's gear.ui and returns the height it used.
    function api.page(fn)
        assert(fn == nil or type(fn) == 'function', 'page must be a function')
        mod.page = fn
    end
    function api.list()
        local out = {}
        for _, e in ipairs(catalog_list) do
            if not e.invalid then out[#out + 1] = mod_info(e) end
        end
        table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
        return out
    end
    function api.get(id)
        local e = catalog[id]
        return e and not e.invalid and mod_info(e) or nil
    end
    -- read(id, relative) -> text | nil, message: a file of an installed mod's folder (read only; this mod's own too).
    function api.read(id, relative)
        assert(type(id) == 'string' and type(relative) == 'string', 'expected a mod id and a path')
        local e = catalog[id]
        if not e or e.invalid then return nil, 'no such mod' end
        return native.read_file(e.dir, relative)
    end
    -- files(id, folder) -> {file name, ...}: the files directly in a folder of an installed mod ('' : the mod's folder), sorted.
    function api.files(id, folder)
        assert(type(id) == 'string', 'expected a mod id')
        local e = catalog[id]
        if not e or e.invalid then return nil, 'no such mod' end
        return native.list_mod_dir(e.dir, folder or '')
    end
    return api
end

-- What a mod's code sees as its globals.
-- Drops (gear.drops; native side gear/drops.cpp, docs/GearDrops.md). Each mod's settings are kept per mod, merged after every tick
-- (multipliers multiply, the first mod in load order that chose a generator wins) and handed to the native side.
-- (one table: the kernel's main chunk is near LuaJIT's limit of 200 locals)
local D = {cfg = {}, dirty = true}   -- cfg: mod -> {chance, find, rarity = {[table id] = mult}, rng = {mode, seed, fn}}; pushed: the generator choice last pushed (so that a change elsewhere does not reseed it)

function D.config(mod)
    local c = D.cfg[mod]
    if not c then
        c = {chance = 1, find = 1, rarity = {}}
        D.cfg[mod] = c
    end
    return c
end

function D.has(event)
    local list = handlers[event]
    if not list then return false end
    for i = 1, #list do
        if not list[i].mod.disabled then return true end
    end
    return false
end

function D.publish()
    if not D.dirty then return end
    D.dirty = false
    local chance, find, rarity, rng = 1, 1, {}, nil
    for _, m in ipairs(mods) do
        local c = D.cfg[m]
        if c and not m.disabled then
            chance = chance * c.chance
            find = find * c.find
            for id, mult in pairs(c.rarity) do rarity[id] = (rarity[id] or 1) * mult end
            if c.rng and not rng then rng = c.rng; D.rng_mod = m end
        end
    end
    native.drops_scales(chance, find)
    native.drops_rarity(rarity)
    D.rng_fn = rng and rng.fn or nil
    local key = rng and (rng.mode .. ':' .. tostring(rng.seed) .. ':' .. tostring(rng.fn) .. ':' .. tostring(rng.nonce)) or ''
    if key ~= D.pushed then
        D.pushed = key
        native.drops_rng(rng and (rng.mode == 'lua' and 2 or 1) or 0, rng ~= nil and rng.seed ~= nil, rng and rng.seed or 0)
    end
    native.drops_handlers(D.has('drops.roll'), D.has('drops.select'), rng ~= nil and rng.mode == 'lua')
end

-- The rarity table's id for a weight key: 16 - quality for the qualities 1 .. 14, 0 and 1 for 'special0' and 'special1' (the rows that hand the
-- roll to the special resource selection). The rows 16 and 17 (plain items) are what is left when no rarer row is hit: their weights change nothing.
function D.id_of(quality)
    if quality == 'special0' then return 0 end
    if quality == 'special1' then return 1 end
    assert(type(quality) == 'number' and quality >= 1 and quality <= 14 and quality == math.floor(quality),
        "a weight key is a quality 1 .. 14, 'special0' or 'special1' (plain items are what is left over: their weight changes nothing)")
    return 16 - quality
end

function D.make(mod)
    local d = {}
    -- chance_scale(mult | nil): multiplies the chance of every drop attempt of creatures (the loot budget).
    function d.chance_scale(mult)
        if mult == nil then mult = 1 end
        assert(type(mult) == 'number' and mult >= 0 and mult <= 1000, 'the multiplier must be a number 0 .. 1000')
        D.config(mod).chance = mult
        D.dirty = true
    end
    -- find_scale(mult | nil): multiplies how much the rarity bonus (magic find) widens the weights of the rarer qualities.
    function d.find_scale(mult)
        if mult == nil then mult = 1 end
        assert(type(mult) == 'number' and mult >= 0 and mult <= 1000, 'the multiplier must be a number 0 .. 1000')
        D.config(mod).find = mult
        D.dirty = true
    end
    -- rarity_weights({[quality | 'special0' | 'special1'] = multiplier} | nil): this mod's multipliers of the rarity weights (replaces its previous ones).
    function d.rarity_weights(weights)
        local c = D.config(mod)
        c.rarity = {}
        if weights ~= nil then
            assert(type(weights) == 'table', 'weights must be a table {[quality] = multiplier}')
            for quality, mult in pairs(weights) do
                assert(type(mult) == 'number' and mult >= 0 and mult <= 1000, 'a weight multiplier must be a number 0 .. 1000')
                c.rarity[D.id_of(quality)] = mult
            end
        end
        D.dirty = true
    end
    -- rng(spec | nil): the generator of the loot rolls: 'fair' (PCG32 without modulo bias), {seed = n} (the same, reproducible), a function
    -- returning a number in [0, 1) per draw, or nil / 'game' for the game's own.
    function d.rng(spec)
        local c = D.config(mod)
        D.nonce = (D.nonce or 0) + 1      -- every call counts as a new choice: a seed is applied again
        if spec == nil or spec == 'game' then
            c.rng = nil
        elseif spec == 'fair' then
            c.rng = {mode = 'fair', nonce = D.nonce}
        elseif type(spec) == 'table' then
            assert(spec.seed == nil or type(spec.seed) == 'number', 'seed must be a number')
            c.rng = {mode = 'fair', seed = spec.seed, nonce = D.nonce}
        elseif type(spec) == 'function' then
            c.rng = {mode = 'lua', fn = spec, owner = mod, nonce = D.nonce}
        else
            error("the generator must be 'fair', 'game', {seed = n} or a function", 2)
        end
        D.dirty = true
    end
    -- random(n) -> integer 0 .. n - 1 from the fair generator (the one 'fair' loot rolls use).
    function d.random(n)
        assert(type(n) == 'number' and n >= 1, 'n must be a number >= 1')
        return native.drops_random(n)
    end
    -- rarity_table() -> { {id, quality, weight, vanilla} }: the game's rarity table now (empty before the first game).
    function d.rarity_table()
        local out = {}
        for i, e in ipairs(native.drops_rarity_table()) do
            out[i] = {id = e.id, quality = (e.id >= 2 and e.id <= 15) and (16 - e.id) or 0, weight = e.weight, vanilla = e.vanilla,
                kind = e.id <= 1 and 'special' or e.id <= 15 and 'quality' or 'plain'}
        end
        return out
    end
    -- stats() -> counters of the drop hooks; quality / category are indexed from 0.
    function d.stats()
        local s = native.drops_stats()
        local quality, category = {}, {}
        for i = 1, #s.quality do quality[i - 1] = s.quality[i] end
        for i = 1, #s.category do category[i - 1] = s.category[i] end
        s.quality, s.category = quality, category
        return s
    end
    function d.reset_stats() native.drops_stats_reset() end
    -- simulate(creature, count, bonus?) -> true | nil, message: makes the engine run the loot function `count` times for the creature (an object
    -- id), a few per frame, through all hooks; the items it makes lie in the world. For tuning and tests; single player only.
    function d.simulate(creature, count, bonus, forced)
        assert(type(creature) == 'number' and type(count) == 'number', 'creature and count must be numbers')
        assert(forced == nil or type(forced) == 'number', 'forced must be an item type id')
        if not world.can_change() then return nil, 'world changes are not allowed now (network game, or no game running)' end
        if not native.drops_test_roll(creature, count, bonus or 0, forced or 0) then return nil, 'no such creature, or a simulation is running' end
        return true
    end
    -- simulation_progress() -> done, queued
    function d.simulation_progress() return native.drops_test_progress() end
    return d
end

-- `drops.roll`: a creature's loot function is about to run (forced: the authored item type, 0 for a random drop). A handler gets the event
-- table and may change `bonus` (the rarity bonus) or return false to cancel the roll.
function kernel.drops_roll(ev)
    local list = handlers['drops.roll']
    if not list or #list == 0 then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'drops.roll', h.fn, ev)
            if ok and result == false then ev.cancel = true; break end
        end
    end
    return ev
end

-- `drops.select`: the loot function asks the type picker for an item: a handler may change category, level or quality, or set `type`
-- (an item type id) to use instead of the engine's pick.
function kernel.drops_select(ev)
    local list = handlers['drops.select']
    if not list or #list == 0 then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then call(h.mod, 'drops.select', h.fn, ev) end
    end
    return ev
end

-- A draw of the Lua generator.
function kernel.drops_random()
    local fn, mod = D.rng_fn, D.rng_mod
    if not fn or not mod or mod.disabled then return nil end
    local ok, result = call(mod, 'drops rng', fn)
    if ok and type(result) == 'number' then return result end
    return nil
end
local function make_env(mod)
    local env = {
        assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs, pcall = pcall,
        rawequal = rawequal, rawget = rawget, rawset = rawset, select = select, setmetatable = setmetatable,
        getmetatable = getmetatable, tonumber = tonumber, tostring = tostring, type = type, unpack = unpack,
        xpcall = xpcall, _VERSION = _VERSION,
        string = copy(string), table = copy(table), math = copy(math), bit = copy(bit),
        coroutine = copy(coroutine),
        os = {clock = os.clock, time = os.time, date = os.date, difftime = os.difftime},
    }
    env._G = env

    local function print_(...)
        native.log('[' .. mod.id .. '] ' .. text_of(...))
    end
    env.print = print_

    -- require: Lua files of this mod only ("a.b" -> a\b.lua in the mod's folder), each run once.
    local loaded = {}
    env.require = function(name)
        assert(type(name) == 'string' and name:match('^[%w_%.%-]+$'), 'bad module name')
        if loaded[name] ~= nil then return loaded[name] end
        local relative = name:gsub('%.', '\\') .. '.lua'
        local text, err = native.read_file(mod.dir, relative)
        if not text then error('module ' .. name .. ' not found: ' .. err, 2) end
        local chunk, lerr = native.load(text, '@' .. mod.dir .. '\\' .. relative, env)
        if not chunk then error(lerr, 2) end
        local result = chunk(name)
        if result == nil then result = true end
        loaded[name] = result
        return result
    end

    local gear = {
        version = native.version,
        mod = {id = mod.id, name = mod.name, version = mod.version, dir = mod.dir},
        data = readonly(data),
        json = readonly(json),
        config = mod.config,
        log = print_,
        time = native.time,
        pipe = {open = function(name) return open_pipe(mod, name) end},
        emit = emit,
        events = {
            declare = function(name, info) EV.declare(mod, name, info) end,
            list = EV.list,
            declared = function(name) return EV.declared[name] ~= nil end,
        },
        -- The game's own texts, by the ids its data tables hold.
        text = {
            get = function(id)
                assert(type(id) == 'number', 'id must be a number')
                return native.text_get(id)
            end,
            -- set(id, text): replaces the game's text; text is a string or {en = ..., de = ..., ru = ...} (the game's language, else en,
            -- else any). Returns true, or nil and a reason.
            set = function(id, text)
                assert(type(id) == 'number', 'id must be a number')
                return native.text_set(id, kernel.pick_text(text))
            end,
            -- add(text) -> id | nil, reason: a new text of Gear's own (id from 0x100000) that the game's text lookup serves, for the
            -- names and descriptions of ability copies (`copy.name_text = gear.text.add('Venom')`); same text forms as set.
            add = function(text) return native.text_add(kernel.pick_text(text)) end,
        },
        -- The mod's file overlay (mods/<id>/overlay.txt, applied when Sacred Gear starts).
        files = {
            overlay = function()
                local count, saves = native.overlay_info(mod.id)
                if not count then return nil end
                return {files = count, saves = saves ~= '' and saves or nil}
            end,
            resolve = function(path)
                assert(type(path) == 'string', 'path must be a string')
                return native.overlay_resolve(path)
            end,
            served = function(path)
                assert(type(path) == 'string', 'path must be a string')
                return native.overlay_served(path)
            end,
        },
        -- Counters of the engine hooks that ask the mods for a value (gear/sitehook.cpp).
        hooks = {
            stats = function(name)
                assert(name == 'level.factor' or name == 'combat.constant' or name == 'creature.ability' or name == 'damage' or name == 'effect.add' or name == 'bonus.constant' or name == 'skillslot.select' or name == 'combat.branch' or name == 'skillslot.key', "the hook must be 'level.factor', 'combat.constant', 'creature.ability', 'damage', 'effect.add', 'bonus.constant', 'skillslot.select', 'combat.branch' or 'skillslot.key'")
                local calls, answered, busy = native.site_hooks(name)
                return {calls = calls, answered = answered, busy = busy}
            end,
            sites = function() return native.site_hook_sites() end,
        },
    }
    function gear.after(seconds, fn) return add_timer(mod, seconds, fn, nil) end
    function gear.every(seconds, fn)
        assert(type(seconds) == 'number' and seconds > 0, 'period must be a number > 0')
        return add_timer(mod, seconds, fn, seconds)
    end
    function gear.cancel(handle)
        local t = timer_of[handle]
        if t and t.mod == mod then t.cancelled = true end
    end
    -- A mod's own files: gear.read(name) -> text | nil, message; gear.save(name, text) -> true | nil, message
    -- (names directly in the mod's folder; saving only *.json, *.txt, *.lua other than mod.lua and main.lua).
    function gear.read(name)
        assert(type(name) == 'string', 'name must be a string')
        return native.read_file(mod.dir, name)
    end
    function gear.save(name, text)
        assert(type(name) == 'string' and type(text) == 'string', 'name and text must be strings')
        return native.write_file(mod.dir, name, text)
    end
    gear.task = function(fn) start_task(mod, fn) end
    gear.wait = wait
    gear.wait_until = wait_until
    if automation_on and mod.automation then
        gear.auto = make_auto()
    end
    local w = {}
    for k, v in pairs(world) do w[k] = v end
    w.spawn = function(kind, x, y, opts) return spawn(mod, kind, x, y, opts) end
    -- pickup_rules({block = {ids}, dim = {ids}, dim_alpha = 0..255}): this mod's items the local hero does not pick
    -- up and items whose ground labels are dimmed; replaces the mod's previous rules (nil: none).
    w.pickup_rules = function(rules)
        if rules == nil then
            pickup_rules[mod] = nil
        else
            assert(type(rules) == 'table', 'rules must be a table')
            local r = {block = {}, dim = {}, dim_alpha = math.max(0, math.min(255, math.floor(rules.dim_alpha or 96)))}
            for _, id in ipairs(rules.block or {}) do r.block[id] = true end
            for _, id in ipairs(rules.dim or {}) do r.dim[id] = true end
            pickup_rules[mod] = r
        end
        pickup_dirty = true
    end
    w.pickup_refused = function() return native.pickup_refused() end
    -- w.cursor(exclude): what is under the mouse cursor in the world: {creature = object, x, y, layer, pixel_x, pixel_y}.
    w.cursor = function(exclude)
        local id, x, y, layer, px, py = native.world_cursor(type(exclude) == 'table' and exclude.id or exclude or 0)
        if not id then return nil end
        local obj = id ~= 0 and world.object(id) or nil
        return {object = obj, x = x, y = y, layer = layer, pixel_x = px, pixel_y = py}
    end
    gear.world = w
    -- gear.models: the models' motion slot tables (Models.tmp). `body` is a hero type (1..9) or its name ('seraphim').
    gear.classes = {}
    for k, v in pairs(kernel.classes) do gear.classes[k] = v end
    -- gear.ui.texture_image(name): see meta/ui.lua
    function gear.classes.copy_ability(alias, base, values)
        assert(type(alias) == 'number' and type(base) == 'number', 'expected an alias id (2000..4999 for an art, 100..199 for a spell) and the base ability id')
        if base < 1000 then return nil, 'copies of spells are not supported: the engine does not run a spell under an id of its own (use values.spell to change the spell record of an art that is linked to one)' end
        local spell_values
        if values and values.spell ~= nil then
            spell_values = values.spell
            local plain = {}
            for k, v in pairs(values) do if k ~= 'spell' then plain[k] = v end end
            values = plain
            if type(spell_values) ~= 'table' then return nil, 'values.spell must be a table of spell record fields' end
        end
        local owner = kernel.alias_owner[alias]
        if owner and owner ~= mod then return nil, 'the alias belongs to another mod' end
        if not owner then
            local ok, err = native.alias_create(alias, base)
            if not ok then return nil, err end
            kernel.alias_owner[alias] = mod
        end
        local record = data.abilities[alias]
        if values then
            local written, problems = data.abilities:apply({[alias] = values})
            if not written then return nil, table.concat(problems, '; ') end
        end
        if spell_values then
            -- the numbers of arts like Poison Mist live in a spell record (gear.classes.linked_spell(base)): the copy gets a copy of
            -- that record too (an alias 100..199 that only this art copy uses) and the fields of values.spell go there
            local spell = native.linked_spell(base)
            if spell == 0 then return nil, 'art ' .. base .. ' takes no numbers from a spell record' end
            local sid = kernel.art_spell_alias and kernel.art_spell_alias[alias]
            if not sid then
                for id = 100, 199 do
                    if not kernel.alias_owner[id] then sid = id; break end
                end
                if not sid then return nil, 'no free spell alias (100..199)' end
                local ok, err = native.alias_create(sid, spell)
                if not ok then return nil, err end
                kernel.alias_owner[sid] = mod
                kernel.art_spell_alias = kernel.art_spell_alias or {}
                kernel.art_spell_alias[alias] = sid
                local linked, lerr = native.alias_link(alias, sid)
                if not linked then return nil, lerr end
            end
            local written, problems = data.abilities:apply({[sid] = spell_values})
            if not written then return nil, table.concat(problems, '; ') end
        end
        return record
    end
    gear.classes.target_modes = {free = 0, hostile_creature = 1, self_or_friend = 2, ground_position = 3, corpse = 4, self_centered = 5}
    gear.classes.ability_flags = {no_cast_mounted = 1, mounted_needs_bow = 2, two_handed_only = 4}
    gear.classes.ability_classes = {seraphim = 0x1, gladiator = 0x2, battle_mage = 0x4, dark_elf = 0x8, wood_elf = 0x10, vampiress = 0x20,
        dwarf = 0x400000, daemon = 0x800000}
    -- linked_spell(art_id): the id of the spell record an art takes its numbers from (Poison Mist 1052 -> 12), nil if none.
    function gear.classes.linked_spell(art)
        local s = native.linked_spell(art)
        return s ~= 0 and s or nil
    end
    function gear.classes.remove_ability(alias)
        if kernel.alias_owner[alias] ~= mod then return false end
        local sid = kernel.art_spell_alias and kernel.art_spell_alias[alias]
        if sid then
            native.alias_link(alias, 0)
            kernel.alias_owner[sid] = nil
            native.alias_remove(sid)
            kernel.art_spell_alias[alias] = nil
        end
        kernel.alias_owner[alias] = nil
        return native.alias_remove(alias)
    end
    gear.models = {}
    local function body_type(body)
        if type(body) == 'number' then return body end
        for _, c in ipairs(CLASSES) do if c.name == body then return c.id end end
        error('unknown body ' .. tostring(body), 3)
    end
    -- has_motion(body, slot): whether the body's model has a clip for the slot (0..255).
    function gear.models.has_motion(body, slot) return native.model_has_motion(body_type(body), slot) end
    -- motion(body, slot): the motion record index the slot maps to (0: none), nil if the model was not found.
    function gear.models.motion(body, slot) return native.model_motion(body_type(body), slot) end
    -- set_motion(body, slot, record): maps the slot of the body's model to a motion record (the index motion() of any body
    -- returns, so a body can play another's clip); for every creature of that model, until restore() or the game ends.
    function gear.models.set_motion(body, slot, record)
        local t = body_type(body)
        local saved = kernel.model_saved
        if not saved then saved = {}; kernel.model_saved = saved end
        local key = t * 256 + slot
        if saved[key] == nil then saved[key] = native.model_motion(t, slot) or false end
        return native.model_set_motion(t, slot, record)
    end
    function gear.models.restore()
        for key, was in pairs(kernel.model_saved or {}) do
            if was then native.model_set_motion(math.floor(key / 256), key % 256, was) end
        end
        kernel.model_saved = nil
    end
    gear.party = {}
    for k, v in pairs(kernel.party) do gear.party[k] = v end
    gear.buffs = {STATS = kernel.buffs.STATS, FACTORS = kernel.buffs.FACTORS, OVERLAYS = kernel.buffs.OVERLAYS,
        define = function(def) return kernel.buffs.define(mod, def) end, definitions = kernel.buffs.definitions,
        apply = function(target, buff, opts) return kernel.buffs.apply(mod, target, buff, opts) end,
        remove = kernel.buffs.remove, list = kernel.buffs.list, fields = kernel.buffs.fields,
        change = kernel.buffs.change, overlay = kernel.buffs.overlay, set_overlay = kernel.buffs.set_overlay}
    -- Hero files (the network game's Save\HeroNN.pax and the mod's own heroes\*.pax): list, bring one into the party,
    -- write the controlled hero to one.
    gear.party.hero_files = function()
        local list = native.party_hero_files(mod.dir)
        for _, f in ipairs(list) do
            local c = CLASS_INFO.by_id[f.class_id]
            f.class = c and c.name
        end
        return list
    end
    gear.party.recruit_hero = function(file, opts)
        assert(type(file) == 'string', 'file must be a hero file (hero_files().file)')
        opts = opts or {}
        local leader = native.hero()
        local lo = leader and native.object(leader)
        local x, y, layer = opts.x or (lo and lo.x), opts.y or (lo and lo.y), opts.layer or (lo and lo.layer) or 0
        if not x or not y then return nil, 'no game is running' end
        local id, err = native.party_import(mod.dir, file, math.floor(x), math.floor(y), layer)
        if not id then return nil, err end
        return wrap(id)
    end
    gear.party.export_hero = function(name)
        assert(type(name) == 'string', 'name must be a string')
        return native.party_export(mod.dir, name)
    end
    gear.mp = mp
    gear.net = mp._net.api
    gear.chat = {
        -- available() -> whether the game's message lines can be read (the hook is in).
        available = function() return native.chat_info and (native.chat_info()) or false end,
        -- history(n) -> the last n lines of the session (all kept ones without n), oldest first.
        history = function(n)
            local lines, out = uis.chat.lines, {}
            local first = n and math.max(1, #lines - math.floor(n) + 1) or 1
            for i = first, #lines do out[#out + 1] = lines[i] end
            return out
        end,
        -- replace_overlay() -> available: call it every tick the mod shows the lines itself.
        replace_overlay = function()
            if mod.disabled then return false end
            uis.chat.hide[mod] = uis.chat.tick
            return native.chat_info and (native.chat_info()) or false
        end,
        -- segments(text, color) -> runs {text, color}, plain: a line's backslash codes taken apart.
        segments = function(text, color)
            assert(type(text) == 'string', 'text must be a string')
            return uis.chat_segments(text, color or 0xFFFFFFFF)
        end,
    }
    gear.clipboard = {
        -- set(text) -> true | nil, message: the Windows clipboard gets the text (Unicode, at most 1 MB).
        set = function(text)
            assert(type(text) == 'string', 'text must be a string')
            return native.clipboard_set(text)
        end,
        -- get() -> the text on the clipboard, or nil.
        get = function() return native.clipboard_get() end,
    }
    gear.game = {
        -- difficulty() -> 0..4: the difficulty level of the running game.
        difficulty = function() return native.difficulty() end,
        cursor_click = function(down) return native.cursor_click(down ~= false) end,
        -- can_save() -> true | false, reason: whether a save may start now.
        can_save = function()
            local why = native.save_blocked()
            if why then return false, why end
            return true
        end,
        -- save(slot, name?, opts?) -> true | nil, message: asks the engine to save into Save\GAME<slot>.PAK. The
        -- result comes as 'game.saved' / 'game.save_failed'. opts: {thumbnail = bool, window = bool}; window: as the
        -- open savegame window's Save button (a replacement of the window saves; it closes).
        save = function(slot, name, opts)
            assert(type(slot) == 'number', 'slot must be a number')
            assert(name == nil or type(name) == 'string', 'name must be a string')
            assert(opts == nil or type(opts) == 'table', 'opts must be a table')
            return native.save_request(slot, name or '', opts ~= nil and opts.thumbnail == true,
                opts ~= nil and opts.window == true)
        end,
        -- saves() -> the savegames (Save\GAME*.PAK) by file name, each {slot, file, name, hero, class, class_id,
        -- level, version, compatible, time, mtime, size, world_date, picture}.
        saves = function()
            local list = native.saves()
            for _, s in ipairs(list) do
                local c = CLASS_INFO.by_id[s.class_id]
                s.class = c and c.name or nil
                s.compatible = s.version == 0x1B
                if s.hero == '' then s.hero = nil end
            end
            return list
        end,
        -- save_picture(slot) -> picture id for gear.ui.image (128 x 96), or nil if the savegame has none.
        save_picture = function(slot)
            assert(type(slot) == 'number', 'slot must be a number')
            return native.save_picture(slot)
        end,
        -- load(slot) -> true | nil, message: loads the savegame (single player; in game or from the menus), as the
        -- savegame window's Load does.
        load = function(slot)
            assert(type(slot) == 'number', 'slot must be a number')
            return native.save_load(slot)
        end,
        -- delete_save(slot) -> true | nil, message: deletes Save\GAME<slot>.PAK.
        delete_save = function(slot)
            assert(type(slot) == 'number', 'slot must be a number')
            local ok, err = native.save_delete(slot)
            if ok then
                local scope = native.scope_of(slot)
                for _, m in ipairs(mods) do native.scope_clear(m.dir, scope) end
            end
            return ok, err
        end,
        save_pending = function() return native.save_pending() end,
        -- config(key) -> string | number | nil: a value of the game's Settings.cfg (read only).
        config = function(key)
            assert(type(key) == 'string', 'key must be a string')
            local v = native.game_config()[key:upper()]
            if v == nil then return nil end
            return tonumber(v) or v
        end,
        -- config_all() -> {KEY = string | number}
        config_all = function()
            local out = {}
            for k, v in pairs(native.game_config()) do out[k] = tonumber(v) or v end
            return out
        end,
        save_info = function(slot)
            assert(type(slot) == 'number', 'slot must be a number')
            return native.save_info(slot)
        end,
        -- free_slot() -> the first slot from 1 whose file does not exist (the game's own rule for a new savegame), nil if all
        -- numbers up to 32766 are taken.
        free_slot = function() return native.save_free_slot() end,
        -- reserve_slots(slots) -> nothing: the slots (numbers, at most 64) this mod writes to itself. free_slot and the game's
        -- own "new savegame" leave them alone even while their files do not exist yet. Replaces this mod's earlier list.
        reserve_slots = function(slots)
            assert(type(slots) == 'table', 'slots must be a table of numbers')
            local list = {}
            for _, n in ipairs(slots) do
                if type(n) == 'number' and #list < 64 then list[#list + 1] = n end
            end
            kernel.slot_reservations[mod] = list
            local all = {}
            for _, l in pairs(kernel.slot_reservations) do
                for _, n in ipairs(l) do all[#all + 1] = n end
            end
            native.save_reserve(all)
        end,
    }
    -- gear.savegame: data that belongs to the savegame being played (see the section above).
    local function savegame_use()
        if not mod.savegame_used then
            mod.savegame_used = true
            kernel.savegame.enter(mod)
        end
    end
    gear.savegame = {
        -- scope() -> 'GAME05' | 'HERO03' | nil: the savegame (Save\GAME05.PAK) this game was loaded from or last saved to, the
        -- network hero (Save\Hero03.pax) it was loaded from; nil in a new game that was not saved yet. In another folder
        -- than the game's own Save (ReBorn's mods\reborn\save) it is <folder>-GAME05.
        scope = function() return kernel.savegame.scope end,
        -- kind() -> 'load' | 'hero' | 'new' | nil: how this game started; nil if the game's load command was not seen.
        kind = function() return kernel.savegame.kind end,
        -- scope_of(slot) -> the scope name of savegame slot `slot`.
        scope_of = function(slot)
            assert(type(slot) == 'number', 'slot must be a number')
            return native.scope_of(slot)
        end,
        -- read(name) -> text | nil, message; write(name, text) -> true | nil, message: a file of this mod's data of the
        -- running game (.json or .txt, names of letters, digits, _ - and .). Most mods use gear.store.game instead.
        read = function(name)
            assert(type(name) == 'string', 'name must be a string')
            savegame_use()
            return native.scope_read(mod.dir, kernel.savegame.working, name)
        end,
        write = function(name, text)
            assert(type(name) == 'string' and type(text) == 'string', 'name and text must be strings')
            savegame_use()
            return native.scope_write(mod.dir, kernel.savegame.working, name, text)
        end,
        -- read_saved(scope, name) -> text | nil, message: a file of the data kept for another savegame.
        -- write_saved(scope, name, text) -> true | nil, message: a file of the data kept for savegame `scope` (what the mod
        -- learned after the savegame was written, e.g. when it ends: see the event 'savegame.leaving').
        write_saved = function(scope, name, text)
            assert(type(scope) == 'string' and type(name) == 'string' and type(text) == 'string', 'scope, name and text must be strings')
            if scope == kernel.savegame.working then return nil, 'not a savegame scope' end
            return native.scope_write(mod.dir, scope, name, text)
        end,
        read_saved = function(scope, name)
            assert(type(scope) == 'string' and type(name) == 'string', 'scope and name must be strings')
            return native.scope_read(mod.dir, scope, name)
        end,
    }
    gear.inv = inv
    gear.realm = realm_api
    gear.settings = make_settings_api(mod)
    gear.drops = D.make(mod)
    gear.keys = make_keys_api(mod)
    gear.input = uis.make_input(mod)
    gear.mods = make_mods_api(mod)
    gear.menu = uis.make_menu(mod)
    if mod.builtin then env.admin = admin end
    gear.ui = make_ui(mod)
    -- gear.on(event, fn) -> handle; gear.off(handle). Events: "load" (all mods loaded), "frame" (dt in seconds,
    -- every in-game frame on the engine thread), "session.start" (hero), "session.stop", and the custom events
    -- mods emit.
    function gear.on(event, fn)
        assert(type(event) == 'string', 'event name must be a string')
        assert(type(fn) == 'function', 'handler must be a function')
        local list = handlers[event]
        if not list then list = {}; handlers[event] = list end
        local h = {mod = mod, fn = fn, event = event}
        list[#list + 1] = h
        if event == 'level.factor' then native.level_factor_active(true) end
        if event == 'combat.constant' then native.combat_constant_active(true) end
        if event == 'creature.ability' then native.creature_ability_active(true) end
        if event == 'creature.created' then native.created_active(true) end
        if event == 'ability.route' then native.route_active(true) end
        if event == 'rune.usable' then native.usable_active(true) end
        if event == 'motion.select' then native.motion_active(true) end
        if event == 'damage' or event == 'ability.hit' then native.damage_active(true) end
        if event == 'effect.add' then native.effect_active(true) end
        if event == 'bonus.constant' then native.bonus_active(true) end
        if event == 'skillslot.select' then native.skillslot_active(true) end
        if event == 'skillslot.key' then native.skillkey_active(true) end
        if event == 'combat.branch' then native.branch_active(true) end
        if event == 'drops.roll' or event == 'drops.select' then D.dirty = true end
        return h
    end
    function gear.off(h)
        local list = h and handlers[h.event]
        if not list then return end
        for i = #list, 1, -1 do
            if list[i] == h then table.remove(list, i) end
        end
        if h.event == 'level.factor' and #list == 0 then native.level_factor_active(false) end
        if h.event == 'combat.constant' and #list == 0 then native.combat_constant_active(false) end
        if h.event == 'creature.ability' and #list == 0 then native.creature_ability_active(false) end
        if h.event == 'creature.created' and #list == 0 then native.created_active(false) end
        if h.event == 'ability.route' and #list == 0 then native.route_active(false) end
        if h.event == 'rune.usable' and #list == 0 then native.usable_active(false) end
        if h.event == 'motion.select' and #list == 0 then native.motion_active(false) end
        if (h.event == 'damage' or h.event == 'ability.hit') and #list == 0 then kernel.boss.sync() end
        if h.event == 'effect.add' and #list == 0 then native.effect_active(false) end
        if h.event == 'bonus.constant' and #list == 0 then native.bonus_active(false) end
        if h.event == 'skillslot.select' and #list == 0 then native.skillslot_active(false) end
        if h.event == 'skillslot.key' and #list == 0 then native.skillkey_active(false) end
        if h.event == 'combat.branch' and #list == 0 then native.branch_active(false) end
        if h.event == 'drops.roll' or h.event == 'drops.select' then D.dirty = true end
    end
    -- gear.guard(where, fn) -> a function that calls fn and catches its errors: the first error of each `where` is logged,
    -- later ones are not, and none counts against the mod (a broken draw does not switch the whole mod off).
    local guard_warned = {}
    function gear.guard(where, fn)
        assert(type(fn) == 'function', 'guard(where, fn)')
        where = tostring(where)
        return function(...)
            local ok, err = xpcall(fn, traceback, ...)
            if not ok and not guard_warned[where] then
                guard_warned[where] = true
                log('[%s] %s failed: %s', mod.id, where, tostring(err))
            end
            if ok then return err end
        end
    end
    env.gear = gear
    -- The libraries shipped in the DLL (lua/<name>.lua): each is run in this mod's environment the first time the mod
    -- touches gear.<name>, so a mod that never uses one pays nothing for it.
    local LIBRARIES = {lookup = true, fmt = true, toast = true, store = true}
    local library_failed = {}
    setmetatable(gear, {__index = function(t, name)
        if not LIBRARIES[name] or library_failed[name] then return nil end
        local source = native.builtin and native.builtin(name)
        local chunk, err = nil, 'not in this SacredGear.dll'
        if source then chunk, err = native.load(source, '@gear/' .. name .. '.lua', env) end
        local ok, lib = false, err
        if chunk then ok, lib = pcall(chunk) end
        if not ok or lib == nil then
            library_failed[name] = true
            native.log('[' .. mod.id .. '] gear.' .. name .. ': ' .. tostring(lib))
            return nil
        end
        rawset(t, name, lib)
        return lib
    end})
    return env
end

-- mod.lua's table, checked: the mod's record, or nil and a message.
local function check_manifest(m, dir)
    if type(m) ~= 'table' then return nil, 'mod.lua must return a table' end
    if type(m.id) ~= 'string' or not m.id:match('^[%w_%-]+$') then
        return nil, 'mod.lua: id must be letters, digits, _ or -'
    end
    if m.version ~= nil and type(m.version) ~= 'string' then return nil, 'mod.lua: version must be a string' end
    if m.priority ~= nil and type(m.priority) ~= 'number' then return nil, 'mod.lua: priority must be a number' end
    if m.requires ~= nil and type(m.requires) ~= 'table' then return nil, 'mod.lua: requires must be a list' end
    if m.pipes ~= nil and type(m.pipes) ~= 'table' then return nil, 'mod.lua: pipes must be a list' end
    if m.config ~= nil and type(m.config) ~= 'table' then return nil, 'mod.lua: config must be a table' end
    if m.description ~= nil and type(m.description) ~= 'string' then return nil, 'mod.lua: description must be a string' end
    if m.author ~= nil and type(m.author) ~= 'string' then return nil, 'mod.lua: author must be a string' end
    if m.theme ~= nil and (type(m.theme) ~= 'string' or m.theme:find('%.%.') or m.theme:find('^[/\\]') or m.theme:find(':')) then
        return nil, 'mod.lua: theme must be the path of an .rcss file inside the mod'
    end
    local allowed = {}
    for _, name in ipairs(m.pipes or {}) do
        if type(name) ~= 'string' or not name:match('^[%w_%-%.]+$') then
            return nil, 'mod.lua: pipe names are letters, digits, _ - and .'
        end
        allowed[name] = true
    end
    local config = {}
    for k, v in pairs(m.config or {}) do config[k] = v end
    if m.automation ~= nil and type(m.automation) ~= 'boolean' then return nil, 'mod.lua: automation must be true or false' end
    local settings, settings_by_id = parse_settings(m.settings)
    if not settings then return nil, settings_by_id end
    local keys, keys_by_id = parse_keys(m.keys)
    if not keys then return nil, keys_by_id end
    return {
        automation = m.automation == true,
        pipes = allowed,
        config = config,
        id = m.id,
        name = type(m.name) == 'string' and m.name or m.id,
        version = m.version or '0',
        description = m.description or '',
        author = m.author or '',
        priority = m.priority or 0,
        requires = m.requires or {},
        enabled = m.enabled ~= false,
        settings = settings,
        settings_by_id = settings_by_id,
        keys = keys,
        keys_by_id = keys_by_id,
        theme = m.theme,
        dir = dir,
    }
end

-- mod.lua: a table literal (or a chunk that returns one) evaluated with no globals at all.
local function read_manifest(dir)
    local text, err = native.read_file(dir, 'mod.lua')
    if not text then return nil, err end
    local chunk, lerr = native.load(text, '@' .. dir .. '\\mod.lua', {})
    if not chunk then return nil, lerr end
    local ok, m = pcall(chunk)
    if not ok then return nil, tostring(m) end
    return check_manifest(m, dir)
end

-- Mods in load order: by priority (lower first), then id; a mod after the mods it requires. Mods whose
-- requirements are missing are left out.
local function order(found, have)
    table.sort(found, function(a, b)
        if a.priority ~= b.priority then return a.priority < b.priority end
        return a.id < b.id
    end)
    local by_id = {}
    for _, m in ipairs(found) do by_id[m.id] = m end
    local result, state = {}, {}
    local function visit(m, chain)
        if state[m] == 'done' then return true end
        if state[m] == 'visiting' then
            log('mod %s: circular requirement (%s)', m.id, chain)
            return false
        end
        state[m] = 'visiting'
        for _, dep in ipairs(m.requires) do
            local d = by_id[dep]
            if not d and have and have[dep] then
                -- already running
            elseif not d then
                log('mod %s requires %s, which is not installed: skipped', m.id, tostring(dep))
                state[m] = 'failed'
                return false
            elseif not visit(d, chain .. ' -> ' .. dep) then
                log('mod %s: requirement %s failed: skipped', m.id, dep)
                state[m] = 'failed'
                return false
            end
        end
        state[m] = 'done'
        result[#result + 1] = m
        return true
    end
    for _, m in ipairs(found) do
        if state[m] == nil then visit(m, m.id) end
    end
    return result
end

-- FNV-1a over "id@version;" of the loaded mods in load order: the mod set's identity, for checking that the peers
-- of a multiplayer game run the same mods (later milestone).
local function mod_set_hash()
    local M = 4294967296
    local h = 0x811C9DC5
    for _, m in ipairs(mods) do
      if not m.builtin then
        local s = m.id .. '@' .. m.version .. ';'
        for i = 1, #s do
            h = bit.bxor(h, s:byte(i)) % M
            -- h * 0x01000193 mod 2^32, exactly: the prime is 2^24 + 0x193.
            h = (bit.lshift(h, 24) % M + h * 0x193) % M
        end
      end
    end
    return bit.tohex(h)
end
kernel.mod_set_hash = mod_set_hash

---------------------------------------------------------------------------------------------------------------
-- The catalog and the loader: every installed mod has an entry; mods are loaded and unloaded live as the player
-- switches them on and off (mods\gear_mods.json keeps the choice).
---------------------------------------------------------------------------------------------------------------

-- The mod manager screen is a mod of its own, written in Lua and shipped in the DLL (native.builtin).
local BUILTIN_MANIFEST = {
    id = 'gear-mods', name = 'Mod manager', version = native.version, author = 'Sacred Gear',
    description = 'Lists the installed mods: switch them on and off, change their settings and key binds.',
    priority = 1000,
    keys = {{id = 'open', label = 'Open the mod manager', default = 'f11'}},
}

local function new_entry(m, dir, extra)
    local e = {id = m.id, dir = dir, m = m, default_enabled = m.enabled}
    for k, v in pairs(extra or {}) do e[k] = v end
    catalog[e.id] = e
    catalog_list[#catalog_list + 1] = e
    return e
end

-- Reads mods\<dir>: the manifest, with config.lua (optional, values only like mod.lua) laid over `config`, the file a
-- player edits.
local function read_installed(dir)
    local m, err = read_manifest(dir)
    if not m then return nil, err end
    local ctext = native.read_file(m.dir, 'config.lua')
    if ctext then
        local chunk, cerr = native.load(ctext, '@' .. m.dir .. '\\config.lua', {})
        local ok, c = false, cerr
        if chunk then ok, c = pcall(chunk) end
        if ok and type(c) == 'table' then
            for k, v in pairs(c) do m.config[k] = v end
        else
            log('mod %s: config.lua ignored: %s', m.id, ok and 'it must return a table' or tostring(c))
        end
    end
    return m
end

local function discover()
    for _, dir in ipairs(native.mod_dirs()) do
        local m, err = read_installed(dir)
        if not m then
            log('mods\\%s: %s', dir, err)
            local id = '?' .. dir
            local stub = {id = id, name = dir, version = '', requires = {}, keys = {}, settings = {}, config = {},
                          enabled = false, dir = dir, settings_by_id = {}, keys_by_id = {}}
            new_entry(stub, dir, {invalid = true, problem = err})
        elseif catalog[m.id] then
            log('mod %s in mods\\%s: the same id as mods\\%s: skipped', m.id, dir, catalog[m.id].dir)
        else
            if not m.enabled then log('mod %s (mods\\%s) is disabled', m.id, dir) end
            new_entry(m, dir)
        end
    end
    local source = native.builtin and native.builtin('modmanager')
    if source then
        local m, err = check_manifest(BUILTIN_MANIFEST, '(built in)')
        if m then
            m.builtin = true
            new_entry(m, m.dir, {builtin = true, source = source})
        else
            log('mod manager: %s', err)
        end
    end
    table.sort(catalog_list, function(a, b) return a.id < b.id end)
end

local function load_mod(e, initial)
    local m = e.m
    m.errors, m.disabled, m.unloaded, m.timer_count = 0, nil, nil, 0
    m.entry = e
    e.fail = nil
    m.env = make_env(m)
    local text, err
    if e.builtin then text = e.source else text, err = native.read_file(m.dir, 'main.lua') end
    if not text then
        e.fail = err or 'no source'
        log('mod %s: %s', m.id, e.fail)
        return false
    end
    local chunk, lerr = native.load(text, '@' .. m.dir .. '\\main.lua', m.env)
    if not chunk then
        e.fail = lerr
        log('mod %s: %s', m.id, lerr)
        return false
    end
    mods[#mods + 1] = m
    e.loaded = true
    m.loading = true
    local ok = call(m, 'main.lua', chunk)
    m.loading = nil
    if ok and m.restore_panels then pcall(m.restore_panels) end
    if ok then
        log('mod %s %s loaded (mods\\%s)', m.id, m.version, m.dir)
    else
        m.disabled = true
        log('mod %s: main.lua failed: the mod is switched off', m.id)
    end
    if not initial then
        kernel.dispatch_to(m, 'load')
        if session_hero then kernel.dispatch_to(m, 'session.start', wrap(session_hero)) end
    end
    return true
end

-- Takes a mod out of the running game: it gets 'unload', then its handlers, timers, tasks, pipes, key binds, pickup
-- rules and open widgets are dropped. (What it changed in the world stays.)
local function unload_mod(e)
    local m = e.m
    kernel.dispatch_to(m, 'unload')
    for _, list in pairs(handlers) do
        for i = #list, 1, -1 do
            if list[i].mod == m then table.remove(list, i) end
        end
    end
    for name, d in pairs(EV.declared) do if d.mod == m.id then EV.declared[name] = nil end end
    for i = #key_handlers, 1, -1 do
        if key_handlers[i].mod == m then table.remove(key_handlers, i) end
    end
    for i = #mods, 1, -1 do
        if mods[i] == m then table.remove(mods, i) end
    end
    if pickup_rules[m] then pickup_rules[m] = nil; pickup_dirty = true end
    if D.cfg[m] then D.cfg[m] = nil end
    uis.chat.hide[m] = nil
    D.dirty = true
    ui_keys[m] = nil
    key_capture[m] = nil
    uis.drop_menu(m)
    if uis.rml_docs then
        for id, entry in pairs(uis.rml_docs) do
            if entry.mod == m then uis.rml_docs[id] = nil; native.rml_close(id) end
        end
    end
    if uis.input.rules[m] then uis.input.rules[m] = nil; uis.input.dirty = true end
    for i = #uis.input.handlers, 1, -1 do
        if uis.input.handlers[i].mod == m then table.remove(uis.input.handlers, i) end
    end
    for name, owner in pairs(uis.replace) do
        if owner == m then uis.replace[name] = nil end
    end
    if ui_dropdown and (ui_handle_key[ui_dropdown] or ''):sub(1, #m.id + 1) == m.id .. '\0' then ui_dropdown = nil end
    m.disabled, m.unloaded = true, true     -- timers, tasks and pipes of the mod notice and go
    e.loaded = false
    log('mod %s unloaded', m.id)
end

-- Brings the running mods in line with the player's choices: unloads the ones that are off (or whose requirements
-- are), loads the ones that are on.
local function apply(initial)
    apply_pending = false
    compute_want()
    for i = #mods, 1, -1 do
        local e = mods[i].entry
        if e and not e.want then unload_mod(e) end
    end
    local todo, have = {}, {}
    for _, m in ipairs(mods) do have[m.id] = true end
    for _, e in ipairs(catalog_list) do
        if e.want and not e.loaded then
            local m = e.m
            if not e.builtin then
                local err
                m, err = read_installed(e.dir)
                if m then
                    e.m = m
                else
                    e.fail = err
                    log('mods\\%s: %s', e.dir, err)
                end
            end
            if m then todo[#todo + 1] = m; m.entry = e end
        elseif initial and not e.invalid and e.reason and desired(e) then
            log('mod %s %s: skipped', e.id, e.reason)
        end
    end
    for _, m in ipairs(order(todo, have)) do load_mod(m.entry, initial) end
end

-- The mod manager's own view of everything (gear.mods.admin does not exist: the manager gets the global `admin`).
function admin.entries()
    local out = {}
    for _, e in ipairs(catalog_list) do out[#out + 1] = mod_info(e) end
    table.sort(out, function(a, b)
        if a.builtin ~= b.builtin then return not a.builtin end
        return a.name:lower() < b.name:lower()
    end)
    return out
end

function admin.set_enabled(id, on)
    local e = catalog[id]
    if not e or e.builtin or e.invalid then return nil, 'this mod cannot be switched' end
    if on then
        -- Its requirements are switched on with it.
        local chain, seen = {}, {}
        local function walk(x)
            if seen[x] then return true end
            seen[x] = true
            chain[#chain + 1] = x
            for _, dep in ipairs(x.m.requires) do
                local d = catalog[dep]
                if not d or d.invalid then return nil, x.id .. ' requires ' .. tostring(dep) .. ', which is not installed' end
                local ok, why = walk(d)
                if not ok then return nil, why end
            end
            return true
        end
        local ok, why = walk(e)
        if not ok then return nil, why end
        for _, x in ipairs(chain) do state_of(x.id, true).enabled = true end
    else
        state_of(id, true).enabled = false
    end
    state_dirty = true
    apply_pending = true
    compute_want()
    return true
end

function admin.settings(id) local e = catalog[id]; return e and e.m.settings or {} end
function admin.keys(id) local e = catalog[id]; return e and e.m.keys or {} end
function admin.key_def(id, key) local e = catalog[id]; return e and e.m.keys_by_id[key] end
function admin.get(id, key)
    local e = catalog[id]
    local d = e and e.m.settings_by_id[key]
    if d then return setting_value(e, d) end
end
function admin.default(id, key)
    local e = catalog[id]
    local d = e and e.m.settings_by_id[key]
    if d then return setting_default(e, d) end
end
function admin.set(id, key, value)
    local e = catalog[id]
    local d = e and e.m.settings_by_id[key]
    if not d then return nil, 'no such setting' end
    return settings_set(e, d, value)
end
function admin.reset(id)
    local e = catalog[id]
    if e then settings_reset(e) end
end
function admin.binding(id, key)
    local e = catalog[id]
    local d = e and e.m.keys_by_id[key]
    if d then return key_binding(e, d) end
end
function admin.default_binding(id, key)
    local e = catalog[id]
    local d = e and e.m.keys_by_id[key]
    return d and d.default
end
function admin.set_binding(id, key, text)
    local e = catalog[id]
    local d = e and e.m.keys_by_id[key]
    if not d then return nil, 'no such key' end
    return key_set(e, d, text)
end
function admin.conflicts(id, key)
    local e = catalog[id]
    local d = e and e.m.keys_by_id[key]
    return d and key_conflicts(e, d) or {}
end
-- Draws a mod's own page (gear.mods.page) in the rectangle, as that mod; returns the height used.
function admin.draw_page(id, x, y, w, h)
    local e = catalog[id]
    if not (e and e.loaded and e.m.page) or e.m.disabled then return 0 end
    local ok, used = call(e.m, 'page', e.m.page, x, y, w, h)
    return ok and tonumber(used) or 0
end
-- While the mod manager's screen is open the other mods' hotkeys stay quiet.
function admin.modal(on)
    keys_blocked = on == true
    local e = catalog['gear-mods']
    if e then key_capture[e.m] = keys_blocked and true or nil end
end

-- Rebinding: capture_start() remembers which keys are down; capture() then returns the binding ('ctrl+m') of the first
-- key pressed since, or nil.
local capture_keys, capture_prev, capture_alone = nil, {}, {}
function admin.capture_start()
    capture_alone = {}
    if not capture_keys then
        capture_keys = {}
        local names = {}
        for name in pairs(UI_VK) do names[#names + 1] = name end
        table.sort(names)
        local seen = {}
        for _, name in ipairs(names) do
            local vk = UI_VK[name]
            if not MOD_ALIAS[name] and name ~= 'escape' and not seen[vk] then
                seen[vk] = true
                capture_keys[#capture_keys + 1] = {name = name, vk = vk}
            end
        end
    end
    for _, k in ipairs(capture_keys) do capture_prev[k.vk] = native.key_down(k.vk) end
end
function admin.capture()
    if not capture_keys then admin.capture_start() end
    local hit
    for _, k in ipairs(capture_keys) do
        local down = native.key_down(k.vk)
        if down and not capture_prev[k.vk] and not hit then hit = k end
        capture_prev[k.vk] = down
    end
    if not hit then
        -- A modifier pressed and let go on its own is the binding ('alt'): for what the game polls.
        for _, m in ipairs(MOD_ORDER) do
            local down = native.key_down(UI_VK[m]) == true
            if down then capture_alone[m] = true
            elseif capture_alone[m] then
                capture_alone = {}
                return m
            end
        end
        return nil
    end
    capture_alone = {}
    local parts = {}
    for _, m in ipairs(MOD_ORDER) do
        if native.key_down(UI_VK[m]) then parts[#parts + 1] = m end
    end
    parts[#parts + 1] = hit.name
    return table.concat(parts, '+')
end
function admin.valid_binding(text) return normalize_binding(text) end

-- The controller's mod menu: the pad button was pressed since the last call.
function admin.menu_pressed()
    if ui_pad.menu > 0 then
        ui_pad.menu = 0
        return true
    end
    return false
end
-- Its entries: {{label, mod, run = fn}}: the declared keys of the mods that run (with their binding in the label),
-- then the mods' menu items.
function admin.menu_entries()
    local out = {}
    for _, e in ipairs(catalog_list) do
        if e.loaded and not e.m.disabled then
            for _, h in ipairs(key_handlers) do
                if h.mod == e.m and not (e.builtin and h.id == 'open') then
                    local b = key_binding(e, h.def)
                    local label = h.def.label .. (b ~= '' and ('  [' .. b .. ']') or '')
                    out[#out + 1] = {label = label, mod = e.m.name, run = function() call(h.mod, 'key ' .. h.def.id, h.fn) end}
                end
            end
            for _, it in ipairs(ui_menu_items[e.m] or {}) do
                out[#out + 1] = {label = it.label, mod = e.m.name, run = function() call(e.m, 'menu ' .. it.id, it.fn) end}
            end
        end
    end
    return out
end

function kernel.load()
    state_load()
    discover()
    -- Automation can name the mods to load (-gear-mods=a,b); their requirements are not added.
    if automation_on then
        local only = native.auto_mods()
        if #only > 0 then
            local wanted = {}
            for _, id in ipairs(only) do wanted[id] = true end
            for _, e in ipairs(catalog_list) do
                if not e.builtin and not wanted[e.id] then
                    e.default_enabled = false
                    log('mod %s: not in -gear-mods: skipped', e.id)
                end
            end
        end
    end
    apply(true)
    log('%d mod(s) loaded, mod set %s', #mods, mod_set_hash())
    kernel.dispatch('load')
    return #mods
end

-- Calls the handlers of one mod for `event` (its 'load' when it is switched on live, 'unload', 'settings.changed').
function kernel.dispatch_to(mod, event, ...)
    local list = handlers[event]
    if not list then return end
    for _, h in ipairs({unpack(list)}) do
        if h.mod == mod and not mod.disabled then
            call(mod, event, h.fn, ...)
        end
    end
end

-- Calls every handler of `event` in load order. A handler that errors is logged; others still run.
function kernel.dispatch(event, ...)
    local list = handlers[event]
    if not list then return end
    -- A copy: handlers may add or remove handlers while they run.
    local snapshot = {unpack(list)}
    for i = 1, #snapshot do
        local h = snapshot[i]
        if not h.mod.disabled then
            call(h.mod, event, h.fn, ...)
        end
    end
end

-- Site hooks (gear/sitehook.cpp): the engine asks for a value in the middle of its own code. `level.factor`: the first mod
-- handler that returns a number answers (the integer factor for `level`); `site` says which of the engine's six places
-- asked. Called by the host from the game's own code, never nested in a kernel call (the host refuses that).
function kernel.level_factor(level, site)
    local list = handlers['level.factor']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'level.factor', h.fn, level, site)
            if ok and type(result) == 'number' then
                return math.floor(result + 0.5)
            end
        end
    end
    return nil
end

-- `combat.constant`: the engine loads one of its pooled float constants inside its own code (a hit handler's effect scale, the
-- difficulty factors ...). The first handler that returns a number answers; handlers get (name, vanilla value, index among that
-- name's places). A handler that returns nil leaves the vanilla value.
function kernel.combat_constant(name, vanilla, index)
    local list = handlers['combat.constant']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'combat.constant', h.fn, name, vanilla, index)
            if ok and type(result) == 'number' then return result end
        end
    end
    return nil
end

-- `creature.ability`: the creature AI picks the ability (a skill or spell id) a creature of `type` will use; `vanilla` is the id the engine
-- would pick (-1 where the code computes it and Lua only overrides), `index` and `address` name the place. A handler returns the id to use, or nil.
function kernel.creature_ability(type_id, vanilla, index, address)
    local list = handlers['creature.ability']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'creature.ability', h.fn, type_id, vanilla, index, address)
            if ok and type(result) == 'number' then return math.floor(result) end
        end
    end
    return nil
end

-- `ability.route`: a hero of `type` has the combat art `art` in flight and the engine asks which hero class runs it; `vanilla` is the
-- class it would pick for the type, `action` the creature's action word, `id` the object id. A handler returns a hero type 1..9
-- (the class whose handler runs the art) or nil.
function kernel.ability_route(type_id, art, vanilla, action, id)
    local list = handlers['ability.route']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'ability.route', h.fn, type_id, art, vanilla, action, id)
            if ok and type(result) == 'number' then return math.floor(result) end
        end
    end
    return nil
end

-- `rune.usable`: a hero of `type` is checked for the class mask of `ability` (reading a rune); `vanilla` is the engine's answer. A handler
-- returns true or false, or nil to leave it.
function kernel.rune_usable(type_id, ability, vanilla)
    local list = handlers['rune.usable']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'rune.usable', h.fn, type_id, ability, vanilla)
            if ok and type(result) == 'boolean' then return result end
        end
    end
    return nil
end

-- `motion.select`: a hero of `type` is about to play motion slot `slot`; a handler returns another slot (0..255) or nil.
function kernel.motion_select(type_id, slot, id)
    local list = handlers['motion.select']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'motion.select', h.fn, type_id, slot, id)
            if ok and type(result) == 'number' then return math.floor(result) end
        end
    end
    return nil
end

-- `skillslot.key`: a character key reached the skill slot window's key handler; a handler gets the character code and returns a slot 0 .. 4
-- (the whole case of the digit keys: page choice, COMBINE_SLOTS check, selection) or `{slot, 'select'}` (the selection only), or
-- `false` to ignore the key, or nil for the engine's own routing (digits 1..5 and 6..0 as the engine maps them).
function kernel.skillslot_key(character)
    local list = handlers['skillslot.key']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, r = call(h.mod, 'skillslot.key', h.fn, character)
            if ok and r == false then return -1, 0 end
            if ok and type(r) == 'number' then return r, 0 end
            if ok and type(r) == 'table' and type(r[1]) == 'number' then return r[1], r[2] == 'select' and 1 or 0 end
        end
    end
    return nil
end

-- `combat.branch`: a conditional jump in the hit handlers and a few other functions; a handler gets `(name, vanilla, index, eax)` (`vanilla`:
-- whether the engine's own condition takes the jump, `eax` the register the compare before it used) and returns true (jump) or false (fall through), nil keeps the engine's. The first boolean answers.
function kernel.combat_branch(name, vanilla, index, eax)
    local list = handlers['combat.branch']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'combat.branch', h.fn, name, vanilla, index, eax)
            if ok and type(result) == 'boolean' then return result end
        end
    end
    return nil
end

-- `skillslot.select`: a digit key selected a skill slot (the skill slot window of the HUD); `index` is the place among the five hooks.
function kernel.skillslot_select(index)
    local list = handlers['skillslot.select']
    if not list then return end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then call(h.mod, 'skillslot.select', h.fn, index) end
    end
end

-- `bonus.constant`: a number the item bonus code (`FUN_00571710`) pushes as the argument of a random roll or a text lookup, `index` the
-- place among the sites, `vanilla` the engine's own value, `address` where it is. A handler returns the number to use (an integer, or for the
-- amplitude sites a float) or nil.
function kernel.bonus_constant(index, vanilla, address)
    local list = handlers['bonus.constant']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'bonus.constant', h.fn, index, vanilla, address)
            if ok and type(result) == 'number' then return result end
        end
    end
    return nil
end

-- `effect.add`: the engine adds a value to a creature's effect entry (`FUN_00562330`'s loop, over every entry). A handler gets
-- `e = {creature, ability, effect, flags, counter, value, max, current}` (the creature's id, the ability it is using, the entry's effect id,
-- the creature's flags, its effect counter, the new sum, the entry's maximum and current value) and returns nil (the engine's own store) or
-- a table `{value = sum | nil (do not store), effect = entry to change instead, clamp = true, finish = false, stack = 'set' | 'inc'}`.
function kernel.effect_add(e)
    local list = handlers['effect.add']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'effect.add', h.fn, e)
            if ok and type(result) == 'table' then return result end
        end
    end
    return nil
end

-- `damage`: the final damage of a hit, as the engine is about to apply it (a hook inside its damage resolution). A handler gets
-- `hit = {damage, victim_type, attacker_type, site, victim, attacker}` (creature types, -1 for none; object ids, 0 for none) and may return another damage; the engine keeps 16 bit
-- (0 .. 65534). The first handler that returns a number answers.
function kernel.damage(damage, victim_type, attacker_type, index, victim, attacker)
    local list = handlers['damage']
    local answer = nil
    if list and #list > 0 then
        local hit = {damage = damage, victim_type = victim_type, attacker_type = attacker_type, site = index,
                     victim = victim or 0, attacker = attacker or 0}
        for i = 1, #list do
            local h = list[i]
            if h and not h.mod.disabled then
                local ok, result = call(h.mod, 'damage', h.fn, hit)
                if ok and type(result) == 'number' then answer = math.floor(result); break end
            end
        end
    end
    -- `ability.hit`: a hit of a creature that has a combat art or spell (id from 100: also copies) in flight
    local hits = handlers['ability.hit']
    if hits and #hits > 0 and attacker and attacker ~= 0 and victim and victim ~= 0 then
        local art = native.art_in_flight(attacker)
        if art and art >= 100 then
            local a, v = world.object(attacker), world.object(victim)
            if a and v then
                for i = 1, #hits do
                    local h = hits[i]
                    if h and not h.mod.disabled then call(h.mod, 'ability.hit', h.fn, a, v, art, answer or damage) end
                end
            end
        end
    end
    if kernel.boss.count > 0 then
        return kernel.boss.hit(answer or damage, victim or 0, attacker or 0)
    end
    return answer
end

-- The 'draw' handlers get the UI state (gear.ui.state()); read only when someone draws.
local function gear_ui_state()
    return ui_tick_state
end

---------------------------------------------------------------------------------------------------------------
-- Per savegame data (gear.savegame, gear.store.game). Each mod that uses it keeps its working copy in
-- saves\_current\ of its folder; a save into slot N copies it to saves\GAMEnn\, a load of slot N copies that back (nothing
-- there: empty), a new game empties it. The game's load / new game command is seen by gear/savegames.cpp.
---------------------------------------------------------------------------------------------------------------
kernel.savegame.working = '_current'

function kernel.savegame.each(fn)
    for _, m in ipairs(mods) do
        if m.savegame_used and not m.disabled then fn(m) end
    end
end

-- The working copy of every mod that uses it becomes the data of `scope` (nil: nothing, as in a new game).
function kernel.savegame.enter(m)
    if kernel.savegame.kind == 'load' or kernel.savegame.kind == 'hero' then
        native.scope_copy(m.dir, kernel.savegame.scope, kernel.savegame.working)
    elseif kernel.savegame.kind == 'new' then
        native.scope_clear(m.dir, kernel.savegame.working)
    end
end

-- A savegame was written: each mod saves its changed data (event 'savegame.saving'), then its working copy is kept for it.
function kernel.savegame.store(scope)
    kernel.dispatch('savegame.saving', scope)
    kernel.savegame.each(function(m) native.scope_copy(m.dir, kernel.savegame.working, scope) end)
    if kernel.savegame.scope == nil then kernel.savegame.scope = scope end
end

function kernel.savegame.poll()
    if not native.game_command then return end
    while true do
        local kind, number, scope = native.game_command()
        if not kind then return end
        -- Before the working copy is replaced: a mod may still write into the data kept for the game that ends.
        kernel.dispatch('savegame.leaving', kernel.savegame.scope, kind)
        kernel.boss.reset()
        kernel.savegame.kind = kind
        kernel.savegame.scope = kind ~= 'new' and scope or nil
        kernel.savegame.each(kernel.savegame.enter)
        if native.buff_clear then native.buff_clear() end
        kernel.dispatch('savegame.changed', kind, kernel.savegame.scope, number)
    end
end

-- The finished saves (gear.game.save) as 'game.saved' (slot, info) and 'game.save_failed' (slot, message).
local function poll_saves()
    while native.hero_saved do
        local slot, network = native.hero_saved()
        if not slot then break end
        if network then kernel.savegame.store(native.hero_scope_of(slot)) end
        kernel.dispatch('game.hero_saved', slot, network)
    end
    while true do
        local r = native.save_poll()
        if not r then return end
        if r.external then
            kernel.savegame.store(native.scope_of(r.slot))
            kernel.dispatch('game.saved_other', r.slot)
        elseif r.ok then
            kernel.savegame.store(native.scope_of(r.slot))
            kernel.dispatch('game.saved', r.slot, r.info)
        else
            kernel.dispatch('game.save_failed', r.slot, r.message)
        end
    end
end

-- The kills this client sent (gear/netmsg.cpp) as the event 'kill' (ev): killer (a player, nil if a creature killed
-- it), victim (a creature handle), type, type_name (nil if the object is already gone), award (experience),
-- by_local (the local hero killed it).
-- `creature.created`: the creatures the engine's own code made since the last tick (sector fills, spawn tables, scripts; see
-- World::takeCreated). Only polled while a mod listens; the engine's list holds 2048 at most per tick (more are dropped).
function kernel.poll_created()
    local list = handlers['creature.created']
    if not list or #list == 0 then return end
    local made = native.take_created()
    -- what the handlers create (gear.world.spawn, or any engine path they trigger) is not reported again: no event loop
    native.created_suppress(true)
    for i = 1, #made do
        local c = made[i]
        local ok, err = pcall(kernel.dispatch, 'creature.created', {
            creature = wrap(c.id), id = c.id, type = c.type, x = c.x, y = c.y, layer = c.layer,
        })
        if not ok then native.log('creature.created: ' .. tostring(err)) end
    end
    native.created_suppress(false)
end

function kernel.poll_kills()
    while true do
        local k = native.net_kill and native.net_kill()
        if not k then return end
        local o = native.object(k.victim)
        local ev = {
            victim = wrap(k.victim),
            type = o and o.type,
            type_name = o and data.object_types:name(o.type) or nil,
            display_name = o and data.object_types:label(o.type) or nil,
            award = k.award,
            by_local = k.killer ~= 0 and k.killer == native.hero(),
        }
        if k.killer ~= 0 then ev.killer = mp.player(k.killer) end
        kernel.dispatch('kill', ev)
    end
end

-- Chat lines (gear.chat, gear/chat.cpp): every line the game adds to its message list (chat, server and game
-- messages, the player's own lines), kept for the session (the last uis.chat.MAX) and dispatched as 'chat.line'.
-- uis.chat.hide[mod] = the tick a mod last asked to show the lines itself (gear.chat.replace_overlay).
uis.chat = {lines = {}, seq = 0, tick = 0, MAX = 200, hide = {}}

-- The game's backslash codes in a line: `\cAARRGGBB` a colour, `\r` back to the line's colour, `\w` (dropped), `\g`
-- plus 16 characters an inline image (dropped). Returns the runs {text, color} and the plain text.
function uis.chat_segments(text, color)
    local runs, plain, cur, buf = {}, {}, color, {}
    local function flush()
        if #buf > 0 then
            local s, last = table.concat(buf), runs[#runs]
            if last and last.color == cur then last.text = last.text .. s else runs[#runs + 1] = {text = s, color = cur} end
            plain[#plain + 1] = s
            buf = {}
        end
    end
    local i, n = 1, #text
    while i <= n do
        local b = text:find('\\', i, true)
        if not b then buf[#buf + 1] = text:sub(i); break end
        if b > i then buf[#buf + 1] = text:sub(i, b - 1) end
        local code = text:sub(b + 1, b + 1)
        local hex = code == 'c' and text:match('^%x%x%x%x%x%x%x%x', b + 2)
        if hex then
            flush(); cur = tonumber(hex, 16); i = b + 10
        elseif code == 'r' then
            flush(); cur = color; i = b + 2
        elseif code == 'w' then
            i = b + 2
        elseif code == 'g' and #text >= b + 17 then
            i = b + 18
        else
            buf[#buf + 1] = '\\'; i = b + 1
        end
    end
    flush()
    return runs, table.concat(plain)
end

function uis.chat_poll()
    local C = uis.chat
    if not native.chat_take then return end
    for _ = 1, 64 do
        local l = native.chat_take()
        if not l then break end
        C.seq = C.seq + 1
        l.id = C.seq
        l.time = native.time()
        l.runs, l.plain = uis.chat_segments(l.text, l.color)
        local lines = C.lines
        lines[#lines + 1] = l
        if #lines > C.MAX then table.remove(lines, 1) end
        kernel.dispatch('chat.line', l)
    end
end

-- After the draw handlers: the game's list stays empty while a mod that runs asked this tick (or the last).
function uis.chat_publish()
    local C, on = uis.chat, false
    for m, t in pairs(C.hide) do
        if m.disabled then C.hide[m] = nil elseif t >= C.tick - 1 then on = true end
    end
    if native.chat_hide and (on or C.was_on) then native.chat_hide(on) end
    C.was_on = on
    C.tick = C.tick + 1
end

-- One in-game frame: sessions, pipes, timers, the frame handlers, then the draw handlers; the frame's drawing is
-- published at the end.
function kernel.tick(dt)
    if apply_pending then apply(false) end
    kernel.savegame.poll()
    check_session()
    poll_keys()
    uis.input_poll()
    poll_saves()
    uis.chat_poll()
    kernel.poll_kills()
    kernel.poll_created()
    kernel.boss.poll()
    kernel.buffs.poll()
    kernel.poll_mp(dt)
    poll_pipes()
    run_timers()
    uis.publish_replace()
    uis.typed = native.ui_take_typed and native.ui_take_typed() or ''
    ui_begin_tick()
    apply_game_layout()
    run_tasks()
    kernel.dispatch('frame', dt)
    kernel.dispatch('draw', gear_ui_state())
    uis.chat_publish()
    ui_end_tick()
    native.ui_commit()
    save_layout()
    publish_pickup_rules()
    D.publish()
    realm_poll()
    realm_flush()
    state_flush()
    native.ui_block_keys(next(key_capture) ~= nil)
end

-- Automation only: a tick in the menus (every 50 ms while no in-game frame runs), on the automation thread.
function kernel.idle()
    if apply_pending then apply(false) end
    kernel.savegame.poll()
    check_session()
    poll_keys()
    poll_pipes()
    run_timers()
    uis.publish_replace()
    uis.typed = native.ui_take_typed and native.ui_take_typed() or ''
    ui_begin_tick()
    apply_game_layout()
    run_tasks()
    kernel.dispatch('idle')
    kernel.dispatch('draw', gear_ui_state())
    uis.chat_publish()
    ui_end_tick()
    native.ui_commit()
    save_layout()
    publish_pickup_rules()
    D.publish()
    realm_poll()
    realm_flush()
    state_flush()
    native.ui_block_keys(next(key_capture) ~= nil)
end

-- The mod whose code was running when the host had to stop it (time budget), for the log.
-- Writes gear_mods.json if something changed (the tick does it too).
function kernel.flush()
    realm_flush()
    state_flush()
end

function kernel.current()
    return current and current.id or nil
end

return kernel
