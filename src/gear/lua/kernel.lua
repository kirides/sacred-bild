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

-- Opt-in profiler (environment SACREDGEAR_PROF): per-native, per-handler and per-tick-phase times, Lua memory; dumped to the log every 5 s.
if os.getenv('SACREDGEAR_PROF') then
    local clock, wrapnat = native.time, {}
    local P = {clock = clock, nat = {}, ev = {}, ph = {}, mem = {n = 0, grow = 0, shrink = 0, last = collectgarbage('count'), max = 0}, nm = {}, last = 0, report_at = clock() + 5}
    kernel.prof = P
    P.nogc = os.getenv('SACREDGEAR_PROF') == 'nogc'
    if P.nogc then collectgarbage('stop') end
    local function rec(t, k) local r = t[k]; if not r then r = {n = 0, t = 0, max = 0}; t[k] = r end; return r end
    for name, f in pairs(native) do
        if type(f) == 'function' and name ~= 'time' and name ~= 'log' then
            local r = rec(P.nat, name)
            native[name] = function(...)
                local t0 = clock()
                return (function(...)
                    local d = clock() - t0
                    r.n = r.n + 1; r.t = r.t + d
                    if d > r.max then r.max = d end
                    local cur = kernel.current and kernel.current()
                    if cur then
                        local m = P.nm[cur]; if not m then m = {}; P.nm[cur] = m end
                        local x = rec(m, name); x.n = x.n + 1; x.t = x.t + d
                    end
                    return ...
                end)(f(...))
            end
        end
    end
    P.lastkb = 0
    function P.begin() P.last = clock(); P.lastkb = collectgarbage('count') end
    function P.mark(name)
        local t = clock()
        local r = rec(P.ph, name)
        local d = t - P.last
        local kb = collectgarbage('count')
        if kb > P.lastkb then r.kb = (r.kb or 0) + kb - P.lastkb end
        P.lastkb = kb
        r.n = r.n + 1; r.t = r.t + d
        if d > r.max then r.max = d end
        P.last = t
    end
    function P.event(mod, what, d, kb0)
        local m = P.ev[mod.id]; if not m then m = {}; P.ev[mod.id] = m end
        local r = rec(m, what)
        local kb = collectgarbage('count') - kb0
        if kb > 0 then r.kb = (r.kb or 0) + kb end
        r.n = r.n + 1; r.t = r.t + d
        if d > r.max then r.max = d end
    end
    function P.memtick()
        local c, M = collectgarbage('count'), P.mem
        M.n = M.n + 1
        if c > M.last then M.grow = M.grow + (c - M.last) else M.shrink = M.shrink + (M.last - c) end
        M.last = c
        if c > M.max then M.max = c end
    end
    function P.dump()
        local t = clock()
        if t < P.report_at then return end
        P.report_at = t + 5
        local M = P.mem
        native.log(('PROFL|%.3f|mem|%d|%.1f|%.1f|%.1f|%.1f'):format(t, M.n, M.last, M.max, M.grow, M.shrink))
        for k, r in pairs(P.nat) do if r.n > 0 then native.log(('PROFL|%.3f|nat|%s|%d|%.4f|%.4f'):format(t, k, r.n, r.t * 1000, r.max * 1000)) end end
        for k, r in pairs(P.ph) do native.log(('PROFL|%.3f|ph|%s|%d|%.4f|%.4f|%.1f'):format(t, k, r.n, r.t * 1000, r.max * 1000, r.kb or 0)) end
        for id, m in pairs(P.ev) do for k, r in pairs(m) do native.log(('PROFL|%.3f|ev|%s/%s|%d|%.4f|%.4f|%.1f'):format(t, id, k, r.n, r.t * 1000, r.max * 1000, r.kb or 0)) end end
        for id, m in pairs(P.nm) do for k, r in pairs(m) do native.log(('PROFL|%.3f|nm|%s/%s|%d|%.4f|0'):format(t, id, k, r.n, r.t * 1000)) end end
    end
end

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
    local pr = kernel.prof
    local t0 = pr and pr.clock()
    local kb0 = pr and collectgarbage('count')
    local ok, result = xpcall(fn, traceback, ...)
    if pr then pr.event(mod, what, pr.clock() - t0, kb0) end
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

