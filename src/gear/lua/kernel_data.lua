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

