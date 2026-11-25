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

