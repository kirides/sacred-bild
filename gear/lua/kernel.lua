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
--   set_health(id, hp), set_gold(id, gold)  true, or nil and a message
--   pipe_open(name), pipe_close(h), pipe_send(h, text), pipe_poll(h, limit), pipe_connected(h)
--   version                           Sacred Gear's version string

local native = ...
local kernel = {}

local MAX_ERRORS = 10   -- a mod whose handlers raised this many errors is switched off for the session

local mods = {}         -- loaded mods in load order: {id, name, version, dir, env, errors, disabled}
local handlers = {}     -- event name -> array of {mod, fn}
local current           -- the mod whose code runs at the moment (for messages)

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

function data.object_types:kind(id)
    local r = self:get(id)
    return r and r:u8(OBJECT_TYPE_KIND)
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

-- Read-only views of kernel tables: mods share `data`, `json` and the world functions, so none may change them
-- for the others. Nested tables are wrapped too (once each).
local frozen = setmetatable({}, {__mode = 'k'})
local function readonly(t)
    if type(t) ~= 'table' then return t end
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
function creature_mt:position() local o = info(self); if o then return o.x, o.y, o.layer end end
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
    return native.set_gold(self.id, math.max(0, math.floor(gold)))
end
function creature_mt:add_gold(amount)
    assert(type(amount) == 'number', 'amount must be a number')
    local o = info(self)
    if not o or not o.gold then return nil, 'no such hero' end
    return native.set_gold(self.id, math.min(math.max(0, o.gold + math.floor(amount)), 4294967295))
end
function creature_mt:teleport(x, y, layer)
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    local o = info(self)
    return native.teleport(self.id, math.floor(x), math.floor(y), layer or (o and o.layer) or 0)
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
local CLASS_BY_ID, CLASS_BY_NAME = {}, {}
for _, c in ipairs(CLASSES) do CLASS_BY_ID[c.id] = c; CLASS_BY_NAME[c.name] = c end
local ALL_CLASSES = 0xC0003F
world.CLASSES = CLASSES

-- The hero's class: name ('darkelf' ...) and id, nil for other creatures.
function creature_mt:class()
    local id = native.hero_class(self.id)
    local c = id and CLASS_BY_ID[id]
    if c then return c.name, c.id end
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
    if it.class_mask ~= 0 and bit.band(it.class_mask, ALL_CLASSES) ~= ALL_CLASSES then
        for _, c in ipairs(CLASSES) do
            if bit.band(it.class_mask, c.bit) ~= 0 then classes[#classes + 1] = c.name end
        end
    end
    it.classes = classes
    it.quest = bit.band(it.type_flags, 0x1000000) ~= 0
    if it.category == 'potion' then it.potion, it.potion_color = potion_kind(it.type) end
    return it
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
-- opts.layer (default: the hero's), opts.label (a text kept for the creature, see creature:label()).
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

local function ui_count(mod)
    local n = (ui_counts[mod] or 0) + 1
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
local ui_panel_list = {}    -- the panels of all mods drawn this tick, in order: {key, title, x, y, w, h, ...}
local ui_panel_prev = {}    -- the last tick's

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
    ui_tick_state = native.ui_state()
    ui_mouse.x, ui_mouse.y = ui_tick_state.mouse_x or 0, ui_tick_state.mouse_y or 0
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
local function ui_end_tick()
    local t = ui_tooltip
    if not t then return end
    local x, y = ui_mouse.x + 18, ui_mouse.y + 20
    local w = (ui_measure(t.text, {}) or (#t.text * 6)) + 12
    local s = ui_tick_state or {}
    if s.screen_right and x + w > s.screen_right then x = ui_mouse.x - w - 4 end
    native.ui_rect(2, -1, x, y, w, 20, 0xE0101018)
    native.ui_rect(2, -1, x, y, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y + 19, w, 1, 0xFFC8AA5A)
    native.ui_rect(2, -1, x, y, 1, 20, 0xFFC8AA5A)
    native.ui_rect(2, -1, x + w - 1, y, 1, 20, 0xFFC8AA5A)
    native.ui_text(2, -1, t.text, x + 6, y, 0, 20, 0xFFF0F0F0, -1, 0, 0, 0, nil, UI_TEXT_SIZE, 0, 1)
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

local function layout_set(key, entry)
    layout_get()[key] = entry
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
}
for i = 1, 12 do UI_VK['f' .. i] = 0x6F + i end
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
    -- ui.image(id, x, y, w, h, {fit = 'stretch' | 'contain' | 'cover', color, layer, window}); w, h default to the
    -- image's own size.
    function ui.image(id, x, y, w, h, opts)
        opts = opts or {}
        local layer, slot = ui_target(opts)
        if not layer then return false end
        local fit = UI_FITS[opts.fit or 'stretch']
        if not fit then error('fit is "stretch", "contain" or "cover"', 2) end
        ui_count(mod)
        return native.ui_image(layer, slot, id, x, y, w or 0, h or 0, ui_color(opts.color, 0xFFFFFFFF), fit)
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
    function ui.fonts() return native.ui_font_count() end

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
        local handle = ui_handle(mod, id)
        ui_count(mod)
        native.ui_region(layer, x, y, w, h, handle)
        ui_regions[#ui_regions + 1] = {handle = handle, layer = layer, x = x, y = y, w = w, h = h}
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

    -- The cursor in layout coordinates, and whether the left button is held over a mod's widget.
    function ui.mouse() return ui_mouse.x, ui_mouse.y end

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

    local function box(x, y, w, h, fill, edge, opts)
        ui.rect(x, y, w, h, fill, opts)
        ui.frame(x, y, w, h, edge, 1, opts)
    end
    local function label_color(opts) return opts.enabled == false and C.text_dim or (opts.color or C.text) end

    -- ui.label(text, x, y, opts): text with the widgets' look (the UI font unless opts.font), a tooltip if opts.tooltip.
    function ui.label(id, text, x, y, opts)
        opts = opts or {}
        local w, h = ui_measure(tostring(text), opts)
        if opts.tooltip then region(id, x, y, w or 60, h or 14, opts) end
        return ui.text(text, x, y, {font = opts.font, size = opts.size, face = opts.face, bold = opts.bold,
            height = opts.height, valign = opts.valign, color = label_color(opts), layer = opts.layer})
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

    -- ui.dropdown(id, x, y, w, options, index, opts) -> changed, index. The open list lies on the top layer; a
    -- click anywhere else closes it.
    function ui.dropdown(id, x, y, w, options, index, opts)
        opts = opts or {}
        local h = 18
        local handle = ui_handle(mod, id)
        local open = ui_dropdown == handle
        local r = region(id, x, y, w, h, opts)
        box(x, y, w, h, (r.hover or open) and C.bg_hover or C.bg, (r.hover or open) and C.edge or C.edge_dim, opts)
        ui.text(tostring(options[index] or ''), x + 6, y, {height = h, valign = 'middle', color = label_color(opts),
            layer = opts.layer})
        -- the arrow
        for i = 0, 3 do ui.rect(x + w - 14 + i, y + 7 + i, 8 - 2 * i, 1, C.accent, opts) end
        if r.clicked then
            open = not open
            ui_dropdown = open and handle or nil
        end
        local changed = false
        if open then
            local s = ui_tick_state or {}
            local top = {layer = 'top'}
            -- Behind the list: the whole screen, so a click beside it closes it (and goes nowhere else).
            local away = region(tostring(id) .. '#away', s.screen_left or 0, s.screen_top or 0,
                (s.screen_right or 1024) - (s.screen_left or 0), (s.screen_bottom or 768) - (s.screen_top or 0), top)
            local ly = y + h
            box(x, ly, w, #options * 18 + 2, 0xF0141420, C.edge, top)
            for i, text in ipairs(options) do
                local ir = region(tostring(id) .. '#' .. i, x + 1, ly + 1 + (i - 1) * 18, w - 2, 18, top)
                if ir.hover or i == index then
                    ui.rect(x + 1, ly + 1 + (i - 1) * 18, w - 2, 18, ir.hover and 0xFF4A3C28 or 0xFF2A2418, top)
                end
                ui.text(tostring(text), x + 6, ly + 1 + (i - 1) * 18, {height = 18, valign = 'middle', layer = 'top',
                    color = i == index and C.accent or C.text})
                if ir.clicked then
                    changed = i ~= index
                    index = i
                    ui_dropdown = nil
                end
            end
            if away.clicked then ui_dropdown = nil end
        end
        return changed, index
    end

    -- ui.slider(id, x, y, w, min, max, value, opts{step, format}) -> changed, value
    function ui.slider(id, x, y, w, min, max, value, opts)
        opts = opts or {}
        local r = region(id, x, y, w, 16, opts)
        ui.rect(x, y + 7, w, 2, C.edge_dim, opts)
        local function at(v) return x + (w - 10) * ((v - min) / math.max(max - min, 1e-9)) end
        local changed = false
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

    -- ui.panel(id, title, x, y, w, h, opts{closable, draggable, layer}) -> panel {x, y, w, h, open, content_x,
    -- content_y}. A window of the mod's: the title bar drags it (it keeps the position for the session), its X
    -- closes it (panel.open false; ui.show_panel(id) opens it again). Its body takes the mouse, so clicks on it do
    -- not reach the game.
    -- Panels: a mod's windows. The layout keeps each one's place (and size, if resizable) under "mod_id:id".
    local panels = {}
    function ui.show_panel(id, open)
        local p = panels[id] or {}
        panels[id] = p
        p.closed = open == false
    end
    function ui.panel_open(id)
        return not (panels[id] and panels[id].closed)
    end
    function ui.panel(id, title, x, y, w, h, opts)
        opts = opts or {}
        local p = panels[id] or {}
        panels[id] = p
        if p.closed then return {open = false} end
        local key = mod.id .. ':' .. tostring(id)
        local saved = layout_get()[key]
        if saved then
            x, y = from_virtual(saved.x, saved.y)
            if opts.resizable and saved.w then w, h = size_from_virtual(saved.w, saved.h) end
        end
        if p.x then x, y = p.x, p.y end         -- being dragged
        if p.w then w, h = p.w, p.h end         -- being resized
        local min_w, min_h = opts.min_width or 120, opts.min_height or 60
        w, h = math.max(w, min_w), math.max(h, min_h)
        local o = {layer = opts.layer}
        ui.rect(x, y, w, h, opts.background or 0xD80E0E14, o)
        ui.frame(x, y, w, h, ui_edit and C.accent or C.edge, ui_edit and 2 or 1, o)
        ui.rect(x + 1, y + 1, w - 2, 20, C.title, o)
        ui.text(title, x + 8, y + 1, {height = 20, valign = 'middle', color = C.accent, layer = opts.layer})
        region(tostring(id) .. '#body', x, y, w, h, o)
        local function store()
            local vx, vy = to_virtual(x, y)
            local e = {x = vx, y = vy}
            if opts.resizable then e.w, e.h = size_to_virtual(w, h) end
            layout_set(key, e)
        end
        if opts.draggable ~= false then
            local bar = region(tostring(id) .. '#bar', x, y, w - 24, 21, {layer = opts.layer, tooltip = opts.tooltip})
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
            local grip = region(tostring(id) .. '#grip', x + w - 14, y + h - 14, 14, 14, {layer = opts.layer, tooltip = 'Resize'})
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
            if cr.clicked then p.closed = true end
        end
        ui_panel_list[#ui_panel_list + 1] = {key = key, mod = mod.id, id = id, title = tostring(title), x = x, y = y,
            w = w, h = h, resizable = opts.resizable and true or false, min_width = min_w, min_height = min_h,
            layer = opts.layer or 'hud'}
        return {open = true, key = key, x = x, y = y, w = w, h = h, content_x = x + 8, content_y = y + 28,
            content_w = w - 16, content_h = h - 36}
    end

    -- Layout editing (like an "edit mode" in other games): the panels of every mod show their ids and their resize
    -- grips while it is on; a layout editor (gear/examples/layout) moves the game's windows too.
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
        for k, v in pairs(layout_get()) do out[k] = {x = v.x, y = v.y, w = v.w, h = v.h} end
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
    function ui.screen()
        local s = native.ui_state()
        return s.screen_left, s.screen_top, s.screen_right, s.screen_bottom
    end
    return ui
end

---------------------------------------------------------------------------------------------------------------
-- Custom events (gear.emit). Built-in events cannot be emitted by mods.
---------------------------------------------------------------------------------------------------------------
local BUILTIN = {load = true, frame = true, idle = true, draw = true}
local RESERVED_PREFIX = {'session.', 'gear.', 'pipe.'}
local emit_depth = 0

local function emit(event, ...)
    assert(type(event) == 'string' and event:match('^[%w_%-]+%.[%w_%.%-]+$'),
        'custom event names have a dot ("twitch.command")')
    assert(not BUILTIN[event], 'built-in event')
    for _, p in ipairs(RESERVED_PREFIX) do
        assert(event:sub(1, #p) ~= p, 'events starting with ' .. p .. ' are reserved')
    end
    if emit_depth >= 8 then error('events nested too deeply', 2) end
    emit_depth = emit_depth + 1
    local ok, err = pcall(kernel.dispatch, event, ...)
    emit_depth = emit_depth - 1
    if not ok then error(err, 0) end
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
    lbutton = 0x01, rbutton = 0x02, mbutton = 0x04, back = 0x08, tab = 0x09, enter = 0x0D, shift = 0x10,
    control = 0x11, alt = 0x12, pause = 0x13, escape = 0x1B, space = 0x20, left = 0x25, up = 0x26, right = 0x27,
    down = 0x28, delete = 0x2E, f1 = 0x70, f2 = 0x71, f3 = 0x72, f4 = 0x73, f5 = 0x74, f6 = 0x75, f7 = 0x76,
    f8 = 0x77, f9 = 0x78, f10 = 0x79, f11 = 0x7A, f12 = 0x7B,
}
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
    function auto.up(button) native.auto_button(vk_of(button or 'lbutton'), false) end
    function auto.key_down(key) native.auto_key(vk_of(key), true) end
    function auto.key_up(key) native.auto_key(vk_of(key), false) end
    function auto.release() native.auto_release() end
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

-- What a mod's code sees as its globals.
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
    w.pickup_refused = function() return native.pickup_refused() end    gear.world = w
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
        return h
    end
    function gear.off(h)
        local list = h and handlers[h.event]
        if not list then return end
        for i = #list, 1, -1 do
            if list[i] == h then table.remove(list, i) end
        end
    end
    env.gear = gear
    return env
end

-- mod.lua: a table literal (or a chunk that returns one) evaluated with no globals at all.
local function read_manifest(dir)
    local text, err = native.read_file(dir, 'mod.lua')
    if not text then return nil, err end
    local chunk, lerr = native.load(text, '@' .. dir .. '\\mod.lua', {})
    if not chunk then return nil, lerr end
    local ok, m = pcall(chunk)
    if not ok then return nil, tostring(m) end
    if type(m) ~= 'table' then return nil, 'mod.lua must return a table' end
    if type(m.id) ~= 'string' or not m.id:match('^[%w_%-]+$') then
        return nil, 'mod.lua: id must be letters, digits, _ or -'
    end
    if m.version ~= nil and type(m.version) ~= 'string' then return nil, 'mod.lua: version must be a string' end
    if m.priority ~= nil and type(m.priority) ~= 'number' then return nil, 'mod.lua: priority must be a number' end
    if m.requires ~= nil and type(m.requires) ~= 'table' then return nil, 'mod.lua: requires must be a list' end
    if m.pipes ~= nil and type(m.pipes) ~= 'table' then return nil, 'mod.lua: pipes must be a list' end
    if m.config ~= nil and type(m.config) ~= 'table' then return nil, 'mod.lua: config must be a table' end
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
    return {
        automation = m.automation == true,
        pipes = allowed,
        config = config,
        id = m.id,
        name = type(m.name) == 'string' and m.name or m.id,
        version = m.version or '0',
        priority = m.priority or 0,
        requires = m.requires or {},
        enabled = m.enabled ~= false,
        dir = dir,
    }
end

-- Mods in load order: by priority (lower first), then id; a mod after the mods it requires. Mods whose
-- requirements are missing are left out.
local function order(found)
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
            if not d then
                log('mod %s requires %s, which is not installed: skipped', m.id, tostring(dep))
                state[m] = 'failed'
                return false
            end
            if not visit(d, chain .. ' -> ' .. dep) then
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
        local s = m.id .. '@' .. m.version .. ';'
        for i = 1, #s do
            h = bit.bxor(h, s:byte(i)) % M
            -- h * 0x01000193 mod 2^32, exactly: the prime is 2^24 + 0x193.
            h = (bit.lshift(h, 24) % M + h * 0x193) % M
        end
    end
    return bit.tohex(h)
end

function kernel.load()
    local found, seen = {}, {}
    for _, dir in ipairs(native.mod_dirs()) do
        local m, err = read_manifest(dir)
        if not m then
            log('mods\\%s: %s', dir, err)
        elseif not m.enabled then
            log('mod %s (mods\\%s) is disabled', m.id, dir)
        elseif seen[m.id] then
            log('mod %s in mods\\%s: the same id as mods\\%s: skipped', m.id, dir, seen[m.id].dir)
        else
            seen[m.id] = m
            found[#found + 1] = m
        end
    end
    -- Automation can name the mods to load (-gear-mods=a,b); their requirements are not added.
    if automation_on then
        local only = native.auto_mods()
        if #only > 0 then
            local wanted = {}
            for _, id in ipairs(only) do wanted[id] = true end
            local kept = {}
            for _, m in ipairs(found) do
                if wanted[m.id] then kept[#kept + 1] = m else log('mod %s: not in -gear-mods: skipped', m.id) end
            end
            found = kept
        end
    end
    for _, m in ipairs(order(found)) do
        m.errors = 0
        -- config.lua (optional, values only like mod.lua) overrides the defaults of mod.lua's `config`: the file a
        -- player edits.
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
        m.env = make_env(m)
        local text, err = native.read_file(m.dir, 'main.lua')
        if not text then
            log('mod %s: %s', m.id, err)
        else
            local chunk, lerr = native.load(text, '@' .. m.dir .. '\\main.lua', m.env)
            if not chunk then
                log('mod %s: %s', m.id, lerr)
            else
                mods[#mods + 1] = m
                local ok = call(m, 'main.lua', chunk)
                if ok then
                    log('mod %s %s loaded (mods\\%s)', m.id, m.version, m.dir)
                else
                    m.disabled = true
                    log('mod %s: main.lua failed: the mod is switched off', m.id)
                end
            end
        end
    end
    log('%d mod(s) loaded, mod set %s', #mods, mod_set_hash())
    kernel.dispatch('load')
    return #mods
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

-- The 'draw' handlers get the UI state (gear.ui.state()); read only when someone draws.
local function gear_ui_state()
    return ui_tick_state
end

-- One in-game frame: sessions, pipes, timers, the frame handlers, then the draw handlers; the frame's drawing is
-- published at the end.
function kernel.tick(dt)
    check_session()
    poll_pipes()
    run_timers()
    ui_begin_tick()
    apply_game_layout()
    run_tasks()
    kernel.dispatch('frame', dt)
    kernel.dispatch('draw', gear_ui_state())
    ui_end_tick()
    native.ui_commit()
    save_layout()
    publish_pickup_rules()
end

-- Automation only: a tick in the menus (every 50 ms while no in-game frame runs), on the automation thread.
function kernel.idle()
    check_session()
    poll_pipes()
    run_timers()
    ui_begin_tick()
    apply_game_layout()
    run_tasks()
    kernel.dispatch('idle')
    kernel.dispatch('draw', gear_ui_state())
    ui_end_tick()
    native.ui_commit()
    save_layout()
    publish_pickup_rules()
end

-- The mod whose code was running when the host had to stop it (time budget), for the log.
function kernel.current()
    return current and current.id or nil
end

return kernel
