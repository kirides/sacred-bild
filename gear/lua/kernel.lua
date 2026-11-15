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
}

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
        data = data,
        log = print_,
        time = native.time,
    }
    -- gear.on(event, fn) -> handle; gear.off(handle). Events: "load" (all mods loaded), "frame" (dt in seconds,
    -- every in-game frame on the engine thread).
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
    return {
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
    for _, m in ipairs(order(found)) do
        m.errors = 0
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

-- The mod whose code was running when the host had to stop it (time budget), for the log.
function kernel.current()
    return current and current.id or nil
end

return kernel
