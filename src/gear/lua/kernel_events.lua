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
        native.clear_pets()
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
    -- auto.nav_kinds() -> {kind, ...}: the kinds of the stops Gear told the controller about last tick (0 button, 1 slider, 2 scroll area, + 4 at its top, + 8 at its bottom).
    function auto.nav_kinds()
        local out, kinds = uis.nav_out or {}, {}
        for i = 1, #out do kinds[i] = out[i].kind end
        return kinds
    end
    -- auto.nav_first() -> x, y: the first stop Gear told the controller about last tick, where the pad's cursor starts when a screen opens (nil: none).
    function auto.nav_first()
        local out = uis.nav_out
        local row = out and out[1]
        if row then return row.x, row.y end
    end
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
    -- auto.pad_page(pages): as LB (-1) / RB (+1) presses of the pad.
    function auto.pad_page(pages) native.auto_page(pages or 0) end
    -- auto.pad_scroll(steps): the D-pad's up (-1) / down (+1) steps in a scroll area, with the game cursor where it is.
    function auto.pad_scroll(steps) native.auto_scroll(steps or 0) end
    -- auto.pad_select(): one press of the Select / "-" button.
    function auto.pad_select() native.auto_page(100) end
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
        for ri = 1, ui_prev_regions.n do
            local r = ui_prev_regions[ri]
            if tostring(r.id) == id and r.mod == mod_id then return r.x, r.y, r.w, r.h end
        end
    end
    -- auto.rml_docs(mod_id?) -> {{mod = id, doc = GearRmlDoc}, ...}: the RmlUi documents that mods have open (of one mod, or all), oldest first, so a test can
    -- walk another mod's tabs and pages: doc:query_all('.tab'), element:rect(), then ui_click.
    function auto.rml_docs(mod_id)
        local out = {}
        for id, e in pairs(uis.rml_docs) do
            if not mod_id or e.mod.id == mod_id then out[#out + 1] = {mod = e.mod.id, doc = e.doc, id = id} end
        end
        table.sort(out, function(x, y) return x.id < y.id end)
        return out
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
    function auto.object_bytes(id, offset, size)
        assert(type(id) == 'number' and type(offset) == 'number' and type(size) == 'number', 'id, offset and size must be numbers')
        return native.auto_object_bytes(id, offset, size)
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

