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
-- Custom events (gear.emit). Built-in events cannot be emitted by mods.
---------------------------------------------------------------------------------------------------------------
local BUILTIN = {load = true, frame = true, idle = true}
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
    local auto = {VK = VK}
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
    gear.task = function(fn) start_task(mod, fn) end
    gear.wait = wait
    gear.wait_until = wait_until
    if automation_on and mod.automation then
        gear.auto = make_auto()
    end
    local w = {}
    for k, v in pairs(world) do w[k] = v end
    w.spawn = function(kind, x, y, opts) return spawn(mod, kind, x, y, opts) end
    gear.world = w
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

-- One in-game frame: sessions, pipes, timers, then the frame handlers.
function kernel.tick(dt)
    check_session()
    poll_pipes()
    run_timers()
    run_tasks()
    kernel.dispatch('frame', dt)
end

-- Automation only: a tick in the menus (every 50 ms while no in-game frame runs), on the automation thread.
function kernel.idle()
    check_session()
    poll_pipes()
    run_timers()
    run_tasks()
    kernel.dispatch('idle')
end

-- The mod whose code was running when the host had to stop it (time budget), for the log.
function kernel.current()
    return current and current.id or nil
end

return kernel
