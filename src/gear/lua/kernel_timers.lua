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

