---------------------------------------------------------------------------------------------------------------
-- The mod manager's data. The catalog lists every installed mod (loaded or not); mods\gear_mods.json keeps what the
-- player chose: enabled flags, setting values and key binds, for all mods in one file. Mods declare settings and
-- keys in mod.lua; the screen that shows them is modmanager.lua (a mod of its own, built into the DLL).
---------------------------------------------------------------------------------------------------------------
local catalog = {}          -- mod id -> entry {id, dir, m, builtin, invalid, loaded, want, reason, fail, excluded}
local catalog_list = {}     -- the entries by id
-- The theme mod: the first running mod (in the catalog's order) whose mod.lua names a `theme` stylesheet. Returns its folder and the file.
-- `dp`: for a document laid out in screen pixels: the stylesheet the manifest names as `theme_dp` (the lengths in dp, the pictures sized in layout units), else `theme`.
function uis.theme_provider(dp)
    for _, e in ipairs(catalog_list) do
        if e.loaded and e.m and e.m.theme then return e.dir, (dp and e.m.theme_dp) or e.m.theme end
    end
end
local mod_state = nil       -- gear_mods.json decoded: {mods = {id = {enabled, settings, keys, migrated}}, order = {id, ...}}
local state_dirty = false
local key_handlers = {}     -- array of {mod, id, def, fn, entry, was}
local keys_blocked = false  -- the mod manager's screen is open: other mods' hotkeys do not fire
local apply_pending = false -- a mod was switched on or off: kernel.tick loads and unloads before it runs the handlers
local admin = {}            -- what only the mod manager sees (filled in below the loader)

local function is_table(v) return type(v) == 'table' and v ~= json.null end

local function state_load()
    if mod_state then return mod_state end
    mod_state = {mods = {}, order = {}}
    local text = native.state_read()
    mod_state.fresh = text == nil   -- no gear_mods.json: the first start
    if text then
        local v, err = json.decode(text)
        if is_table(v) and (v.mods == nil or is_table(v.mods)) then
            for id, e in pairs(v.mods or {}) do
                if is_table(e) then mod_state.mods[id] = e end
            end
            -- The player's load order (the mod manager app writes it): an array of mod ids, first loads first.
            if is_table(v.order) then
                local seen = {}
                for _, id in ipairs(v.order) do
                    if type(id) == 'string' and not seen[id] then
                        seen[id] = true
                        mod_state.order[#mod_state.order + 1] = id
                    end
                end
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
    local file = {version = 1, mods = out}
    if #state_load().order > 0 then file.order = state_load().order end
    if not native.state_write(json.encode(file)) then
        log('gear_mods.json: the file cannot be written')
    end
end

-- Realms ----------------------------------------------------------------------------------------------------------
-- The online play (register / login) server. The choice itself is native (native.realm_select: the game's network menu
-- takes the host and the lobby port from it when Closed or Open Internet opens, see src/gear/realm.h); this keeps the
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
    -- gear.keys.trigger(id): fires the handlers of the declared key as if the player pressed it (a close button of the mod's own window uses it).
    function api.trigger(id)
        def_of(id)
        for _, h in ipairs({unpack(key_handlers)}) do
            if h.mod == mod and h.id == id and not mod.disabled then call(mod, 'key ' .. id, h.fn) end
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

-- A button with data-close-key="<key id>" in a mod's document closes its window: the key's handlers fire (see gear.keys.trigger); the pad's B does the same
-- for the window under the pad's cursor.
function uis.key_trigger(mod, id)
    for _, h in ipairs({unpack(key_handlers)}) do
        if h.mod == mod and h.id == id and not mod.disabled then call(mod, 'key ' .. id, h.fn) end
    end
end

-- gear.input: the game's own keys, remapped (src/gear/ui/input.cpp). State lives in `uis` (the main chunk is at its locals limit).
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
    -- page_rml(path, opts): the same page as an RmlUi document of this mod (a file of the mod's own folder, loaded as the mod: the same path rules,
    -- events and limits as any document the mod loads). opts are the load options (model, calls, on_change ...) plus on_load(doc) once the
    -- document exists (the mod's chance to keep it and fill the model) and update(doc) every frame the page is shown. The document's element
    -- with id "page" is put in the rectangle of the tab (put in it, sized to it: give it overflow: auto if it can be taller); nothing else
    -- of the document is reachable by the host, and it is only drawn while its tab is open.
    function api.page_rml(path, opts)
        assert(type(path) == 'string', 'path must be a string')
        opts = opts or {}
        local doc, failed
        mod.page = function(x, y, w, h)
            if failed then return 0 end
            if not doc then
                local o = {}
                for k, v in pairs(opts) do
                    if k ~= 'on_load' and k ~= 'update' then o[k] = v end
                end
                local err
                doc, err = mod.ui_api.rml.load(path, o)
                if not doc then
                    failed = true
                    log('[%s] mods page %s failed: %s', mod.id, path, tostring(err))
                    return 0
                end
                if opts.on_load then opts.on_load(doc) end
            end
            if opts.update then opts.update(doc) end
            doc:style('#page', 'position', 'absolute')
            native.rml_move(doc.id, '#page', x, y)
            native.rml_resize(doc.id, '#page', w, h)
            doc:draw({layer = 'top'})
            return h
        end
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
-- Drops (gear.drops; native side src/gear/items/drops.cpp, docs/GearDrops.md). Each mod's settings are kept per mod, merged after every tick
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
            -- key(key, text) -> id | nil, reason: a text of Gear's own whose id comes from `key` (a string that is yours alone: 'mymod.smith'),
            -- so it is the same in every session; call it again with the same key to change the text. For what a savegame keeps by id
            -- (a creature's name text, creature:set_name_text): register the key when the mod starts, before a savegame loads.
            key = function(key, text)
                assert(type(key) == 'string' and key ~= '', 'key must be a string')
                return native.text_key(key, kernel.pick_text(text))
            end,
            -- key_of(id) -> the key, if it was registered this session.
            key_of = function(id) return native.text_key_of(id) end,
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
        -- Counters of the engine hooks that ask the mods for a value (src/gear/sitehook.cpp).
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
    gear.buffs = {STATS = kernel.buffs.STATS, FACTORS = kernel.buffs.FACTORS, SPEEDS = kernel.buffs.SPEEDS, OVERLAYS = kernel.buffs.OVERLAYS,
        define = function(def) return kernel.buffs.define(mod, def) end, definitions = kernel.buffs.definitions,
        apply = function(target, buff, opts) return kernel.buffs.apply(mod, target, buff, opts) end,
        remove = kernel.buffs.remove, list = kernel.buffs.list, fields = kernel.buffs.fields, speeds = kernel.buffs.speeds,
        change = kernel.buffs.change, overlay = kernel.buffs.overlay, set_overlay = kernel.buffs.set_overlay}
    gear.pfx = {
        define = function(def) return kernel.pfx.define(mod, def) end, undefine = kernel.pfx.undefine,
        definitions = kernel.pfx.definitions, rule = function(r) return kernel.pfx.rule(mod, r) end,
        remove_rule = kernel.pfx.remove_rule, stats = kernel.pfx.stats}
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
        -- map_markers() -> {{x, y, kind, region}, ...}: the world map's service symbols (merchant 0, blacksmith 1, combo master 2, horse trader 3).
        map_markers = function() return native.map_markers() end,
        cursor_click = function(down) return native.cursor_click(down ~= false) end,
        -- can_save() -> true | false, reason: whether a save may start now.
        can_save = function()
            local why = native.save_blocked()
            if why then return false, why end
            return true
        end,
        -- save(slot, name?, opts?) -> true | nil, message: asks the engine to save into Save\GAME<slot>.PAK. The
        -- result comes as 'game.saved' / 'game.save_failed'. opts: {thumbnail = bool (default true), window = bool}; window: as the
        -- open savegame window's Save button (a replacement of the window saves; it closes).
        save = function(slot, name, opts)
            assert(type(slot) == 'number', 'slot must be a number')
            assert(name == nil or type(name) == 'string', 'name must be a string')
            assert(opts == nil or type(opts) == 'table', 'opts must be a table')
            return native.save_request(slot, name or '', opts == nil or opts.thumbnail ~= false,
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
    gear.shop = kernel.shop_api
    gear.realm = realm_api
    gear.settings = make_settings_api(mod)
    gear.drops = D.make(mod)
    gear.keys = make_keys_api(mod)
    gear.input = uis.make_input(mod)
    gear.mods = make_mods_api(mod)
    gear.menu = uis.make_menu(mod)
    if mod.builtin then env.admin = admin end
    gear.ui = make_ui(mod)
    -- gear.vfs: the files of the mod's folder as the game sees them: on disk, or else in the layers of mod.lua's `vfs` (packs).
    gear.vfs = {
        read = function(path)
            assert(type(path) == 'string', 'path must be a string')
            return native.vfs_read and native.vfs_read(mod.dir, path) or nil
        end,
        exists = function(path)
            assert(type(path) == 'string', 'path must be a string')
            return native.vfs_exists and native.vfs_exists(mod.dir, path) or false
        end,
        list = function(prefix)
            return native.vfs_list and native.vfs_list(mod.dir, prefix or '') or {}
        end,
    }
    mod.ui_api = gear.ui
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
        if event == 'pfx.select' then kernel.pfx.refresh() end
        if event == 'item.use' then native.item_use_active(true) end
        if event == 'kill.recorded' then native.kill_recorded_active(true) end
        if event == 'potion.pick' then native.potion_pick_active(true) end
        if event == 'shop.stock' then native.shop_active(true) end
        if event == 'motion.select' then native.motion_active(true) end
        if event == 'damage' or event == 'ability.hit' then native.damage_active(true) end
        if event == 'effect.add' then native.effect_active(true) end
        if event == 'bonus.constant' then native.bonus_active(true) end
        if event == 'skillslot.select' then native.skillslot_active(true) end
        if event == 'skillslot.key' then native.skillkey_active(true) end
        if event == 'combat.branch' then native.branch_active(true) end
        if event:sub(1, 7) == 'master.' then native.master_active(event, true) end
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
        if h.event == 'pfx.select' then kernel.pfx.refresh() end
        if h.event == 'item.use' and #list == 0 then native.item_use_active(false) end
        if h.event == 'kill.recorded' and #list == 0 then native.kill_recorded_active(false) end
        if h.event == 'potion.pick' and #list == 0 then native.potion_pick_active(false) end
        if h.event == 'shop.stock' and #list == 0 then native.shop_active(false) end
        if h.event == 'motion.select' and #list == 0 then native.motion_active(false) end
        if (h.event == 'damage' or h.event == 'ability.hit') and #list == 0 then kernel.boss.sync() end
        if h.event == 'effect.add' and #list == 0 then native.effect_active(false) end
        if h.event == 'bonus.constant' and #list == 0 then native.bonus_active(false) end
        if h.event == 'skillslot.select' and #list == 0 then native.skillslot_active(false) end
        if h.event == 'skillslot.key' and #list == 0 then native.skillkey_active(false) end
        if h.event == 'combat.branch' and #list == 0 then native.branch_active(false) end
        if h.event:sub(1, 7) == 'master.' and #list == 0 then native.master_active(h.event, false) end
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
    local LIBRARIES = {lookup = true, fmt = true, toast = true, store = true, kit = true, npc = true}
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
    -- m.recommended (ids of mods that go well with this one) is not checked or used here: the external mod manager reads it
    if m.pipes ~= nil and type(m.pipes) ~= 'table' then return nil, 'mod.lua: pipes must be a list' end
    if m.config ~= nil and type(m.config) ~= 'table' then return nil, 'mod.lua: config must be a table' end
    if m.description ~= nil and type(m.description) ~= 'string' then return nil, 'mod.lua: description must be a string' end
    if m.author ~= nil and type(m.author) ~= 'string' then return nil, 'mod.lua: author must be a string' end
    if m.theme ~= nil and (type(m.theme) ~= 'string' or m.theme:find('%.%.') or m.theme:find('^[/\\]') or m.theme:find(':')) then
        return nil, 'mod.lua: theme must be the path of an .rcss file inside the mod'
    end
    if m.exports ~= nil then
        if type(m.exports) ~= 'table' or #m.exports > 64 then return nil, 'mod.lua: exports must be a list of at most 64 paths' end
        for _, x in ipairs(m.exports) do
            if type(x) ~= 'string' or x == '' or x:find('%.%.') or x:find('^[/\\]') or x:find(':') or x:find('\\', 1, true) then
                return nil, 'mod.lua: exports: "*", a folder ("templates/"), a file or "folder/*.ext", inside the mod'
            end
        end
    end
    if m.vfs ~= nil then
        if type(m.vfs) ~= 'table' or #m.vfs > 32 then return nil, 'mod.lua: vfs must be a list of at most 32 layers {file = ..., mount = ...}' end
        for _, layer in ipairs(m.vfs) do
            local f, mount = type(layer) == 'table' and layer.file, type(layer) == 'table' and layer.mount
            if type(f) ~= 'string' or f == '' or f:find('%.%.') or f:find('^[/\\]') or f:find(':') then
                return nil, 'mod.lua: vfs: each layer needs file = the path of a pack inside the mod'
            end
            if mount ~= nil and (type(mount) ~= 'string' or mount:find('%.%.') or mount:find(':')) then
                return nil, 'mod.lua: vfs: mount is a folder inside the mod ("/" or "ui/maps")'
            end
        end
    end
    if m.theme_dp ~= nil and (type(m.theme_dp) ~= 'string' or m.theme_dp:find('%.%.') or m.theme_dp:find('^[/\\]') or m.theme_dp:find(':')) then
        return nil, 'mod.lua: theme_dp must be the path of an .rcss file inside the mod'
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
        theme_dp = m.theme_dp,
        exports = m.exports,
        vfs = m.vfs,
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

-- Mods in load order: the mods in the player's order (gear_mods.json "order") first, in that order; the others by
-- priority (lower first), then id. A mod always comes after the mods it requires. Mods whose requirements are
-- missing are left out.
local function order(found, have)
    local rank = {}
    for i, id in ipairs(state_load().order) do rank[id] = i end
    table.sort(found, function(a, b)
        local ra, rb = rank[a.id], rank[b.id]
        if ra and rb then return ra < rb end
        if ra or rb then return ra ~= nil end
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
    if m.vfs and native.vfs_configure then native.vfs_configure(m.dir, m.vfs) end
    if m.exports and native.rml_exports then native.rml_exports(m.id, m.dir, m.exports) end
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
    if uis.textures then
        for t, owner in pairs(uis.textures) do
            if owner == m then t:release() end
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
    if m.exports and native.rml_exports then native.rml_exports(m.id, m.dir, nil) end
    if native.vfs_unmount then native.vfs_unmount(m.dir) end
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
-- admin.markup() -> the RmlUi document of its screen (src/gear/lua/modmanager.rml, shipped inside the DLL), nil if it is not there.
function admin.markup() return native.builtin and native.builtin('modmanager.rml') end
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
-- The game's focused text field (layout units) and typing into it: the Paste button over the game's text boxes (modmanager.lua).
-- admin.text_focus() -> x, y, w, h | nil;  admin.type_text(text) -> the number of characters sent.
function admin.text_focus()
    if not native.ui_text_focus then return nil end
    return native.ui_text_focus()       -- (not `a and f()`: that would cut the four results to one)
end
function admin.type_text(text) return native.ui_type_text and native.ui_type_text(text) or 0 end

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

-- The mods that are switched on at the very first start (no gear_mods.json); every other installed mod starts off.
function kernel.load()
    local FIRST_RUN_MODS = {['sacred-theme'] = true, ['key-bindings'] = true, ['better-saveload'] = true}
    state_load()
    discover()
    -- First start: only the starter mods run. The choice is written down for every installed mod at once, so a file
    -- that later holds only a setting or key bind does not bring back the defaults; a mod installed after that
    -- follows its own mod.lua `enabled` as before. (Automation runs keep the old behaviour: -gear-mods picks the mods.)
    if mod_state.fresh and not automation_on then
        for _, e in ipairs(catalog_list) do
            if not e.builtin and not e.invalid then
                state_of(e.id, true).enabled = FIRST_RUN_MODS[e.id] == true
            end
        end
        state_dirty = true
        log('first start (no gear_mods.json): only sacred-theme, key-bindings and better-saveload are switched on')
    end
    mod_state.fresh = nil
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

-- Site hooks (src/gear/sitehook.cpp): the engine asks for a value in the middle of its own code. `level.factor`: the first mod
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

-- Skill particle effects (gear.pfx): variants of the engine's own effects (other textures, colours, speed) and the rules that pick one when
-- an art or spell makes its effect (src/gear/world/pfx.cpp). Variant ids are the mods' numbers and shared by every mod, like ability aliases.
kernel.pfx = {defs = {}, rules = {}, seq = 0}
kernel.init_pfx = function()
    local P = kernel.pfx

    local function number_field(def, name, lo, hi)
        local v = def[name]
        if v == nil then return nil end
        assert(type(v) == 'number' and v >= lo and v <= hi, name .. ' must be a number from ' .. lo .. ' to ' .. hi)
        return v
    end

    function P.define(m, def)
        assert(type(def) == 'table', 'define takes a table')
        local id = def.id
        assert(type(id) == 'number' and id >= 1 and id == math.floor(id) and id < 2 ^ 31, 'a variant needs a whole number id of 1 or more')
        local textures = {}
        for k, v in pairs(def.textures or {}) do
            if type(k) == 'number' then
                assert(type(v) == 'table' and type(v[1]) == 'string' and type(v[2]) == 'string', 'textures: {from, to} pairs or [from] = to')
                textures[#textures + 1] = {v[1], v[2]}
            else
                assert(type(k) == 'string' and type(v) == 'string', 'textures: {from, to} pairs or [from] = to')
                textures[#textures + 1] = {k, v}
            end
        end
        local d = {id = id, name = def.name, textures = textures, mod = m.id,
            hue = number_field(def, 'hue', -3600, 3600), colorize = number_field(def, 'colorize', 0, 0xFFFFFF),
            multiply = number_field(def, 'multiply', 0, 0xFFFFFFFF), alpha = number_field(def, 'alpha', 0, 4),
            speed = number_field(def, 'speed', 0.01, 100)}
        local ok, err = native.pfx_define(d)
        assert(ok, err or 'the variant was not accepted')
        P.defs[id] = d
        return id
    end

    function P.undefine(id)
        P.defs[id] = nil
        return native.pfx_undefine(id)
    end

    function P.definitions()
        local out = {}
        for id, d in pairs(P.defs) do out[#out + 1] = d end
        table.sort(out, function(a, b) return a.id < b.id end)
        return out
    end

    local function as_set(v)
        if v == nil then return nil end
        local set = {}
        if type(v) == 'table' then
            for _, x in ipairs(v) do set[x] = true end
        else
            set[v] = true
        end
        return set
    end

    -- The effect types the engine side asks about: all of them if a handler of `pfx.select` or a rule without `fx` exists.
    function P.refresh()
        local all, types = false, {}
        local list = handlers['pfx.select']
        if list and #list > 0 then all = true end
        for _, r in ipairs(P.rules) do
            if not r.fx then
                all = true
            else
                for fx in pairs(r.fx) do types[#types + 1] = fx end
            end
        end
        native.pfx_watch(types, all)
    end

    function P.rule(m, r)
        assert(type(r) == 'table', 'rule takes a table')
        assert(r.use ~= nil or type(r.select) == 'function', 'a rule needs `use` (a variant id) or `select` (a function)')
        assert(r.use == nil or type(r.use) == 'number', '`use` is a variant id')
        P.seq = P.seq + 1
        local rule = {id = P.seq, mod = m, fx = as_set(r.fx), ability = as_set(r.ability), min_level = r.min_level, max_level = r.max_level,
            use = r.use, select = r.select, priority = r.priority or 0}
        P.rules[#P.rules + 1] = rule
        table.sort(P.rules, function(a, b)
            if a.priority ~= b.priority then return a.priority > b.priority end
            return a.id < b.id
        end)
        P.refresh()
        return rule
    end

    function P.remove_rule(rule)
        for i = #P.rules, 1, -1 do
            if P.rules[i] == rule or (type(rule) == 'number' and P.rules[i].id == rule) then table.remove(P.rules, i) end
        end
        P.refresh()
    end

    -- The engine asks at the first update of a new effect of type `fx` (made by `creature_id`, with `ability` in flight, at cast `level`; its parent
    -- effect has the variant `parent`): the id of the variant to use, or nil.
    function kernel.pfx_select(fx, creature_id, ability, level, parent)
        local ctx = {fx = fx, creature = creature_id ~= 0 and wrap(creature_id) or nil, ability = ability ~= 0 and ability or nil,
            level = level > 0 and level or nil, parent = parent ~= 0 and parent or nil}
        if not ctx.level and ctx.creature and ctx.ability then
            -- the first effects of a cast come before the level word is set: the creature's own level of the ability then
            local a = ctx.creature:ability(ctx.ability)
            if a then ctx.level = a.level + a.bonus end
        end
        local list = handlers['pfx.select']
        if list then
            for i = 1, #list do
                local h = list[i]
                if h and not h.mod.disabled then
                    local ok, result = call(h.mod, 'pfx.select', h.fn, ctx)
                    if ok and type(result) == 'number' and P.defs[result] then return result end
                end
            end
        end
        for _, r in ipairs(P.rules) do
            if not r.mod.disabled and (not r.fx or r.fx[fx]) and (not r.ability or (ctx.ability and r.ability[ctx.ability])) then
                local pass = true
                if r.min_level or r.max_level then
                    local lv = ctx.level
                    pass = lv ~= nil and (not r.min_level or lv >= r.min_level) and (not r.max_level or lv <= r.max_level)
                end
                if pass then
                    local id = r.use
                    if r.select then
                        local ok, result = call(r.mod, 'pfx rule', r.select, ctx)
                        id = ok and type(result) == 'number' and result or nil
                    end
                    if id and P.defs[id] then return id end
                end
            end
        end
        return nil
    end

    function P.stats() return native.pfx_stats() end
end
kernel.init_pfx()

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

-- `item.use`: a potion is drunk (item object id, item type, drinker's id); a handler returns 'consume' (no effect, the potion is used up),
-- 'keep' (no effect, the potion stays) or nil (the engine's effect).
function kernel.item_use(item, type_id, creature)
    local list = handlers['item.use']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'item.use', h.fn, {item = item, type = type_id, creature = creature})
            if ok then
                if result == 'consume' then return 1 end
                if result == 'keep' then return 2 end
            end
        end
    end
    return nil
end

-- `potion.pick`: the engine's potion picker is called; the first handler that returns an item type (a number above 0) answers. Called by the host from
-- the engine's own code (merchants, loot), never nested in a kernel call.
function kernel.potion_pick(mode, flag, tier, address, site)
    local list = handlers['potion.pick']
    if not list then return nil end
    local e = {mode = mode, flag = flag, tier = tier, address = address, site = site}
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, 'potion.pick', h.fn, e)
            if ok and type(result) == 'number' and result > 0 then
                return math.floor(result)
            end
        end
    end
    return nil
end

-- `shop.stock`: the engine stocked the merchant window (a new merchant, a restock of the same one, the window's refresh); see src/gear/items/shop.cpp. Called by
-- the host right after the engine's handler returns; the mods add and remove wares through gear.shop.
function kernel.shop_stock(reason, merchant, container, hero_level)
    kernel.dispatch('shop.stock', {reason = reason, merchant = merchant, container = container, hero_level = hero_level})
end

-- `kill.recorded`: the engine's statistics record of a kill by the local hero or by a creature it owns (see src/gear/world/killstat.cpp). Called by the
-- host from inside the engine's death handling, right after the engine's own bookkeeping; `id` is the record's id word.
function kernel.kill_recorded(id)
    kernel.dispatch('kill.recorded', {id = id})
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

-- `master.price`, `master.layout`, `master.exchange`, `master.describe`, `master.maximum`: the combo master's rune exchange tab (src/gear/items/master.cpp).
-- `master.price(rune, ability, count)` returns the gold the exchange costs (an integer, 0 hides the price) or nil for the engine's own;
-- `master.layout()` returns true to take the runes away (hidden slots); `master.exchange(rune, ability, count)` returns true when the mod did the
-- exchange itself; `master.describe(rune, ability, count)` returns the level to show the chosen art's "name - Level n" at (nil or false: no text);
-- `master.maximum(rune, ability, count)` returns true to show the "maximum level" message. The first answer counts.
function kernel.master_event(event, rune, ability, count)
    local list = event == 'master.price' and handlers['master.price'] or event == 'master.layout' and handlers['master.layout'] or
        event == 'master.exchange' and handlers['master.exchange'] or event == 'master.describe' and handlers['master.describe'] or
        event == 'master.maximum' and handlers['master.maximum']
    if not list then return nil end
    for i = 1, #list do
        local h = list[i]
        if h and not h.mod.disabled then
            local ok, result = call(h.mod, event, h.fn, rune, ability, count)
            if ok and (type(result) == 'number' or (type(result) == 'boolean' and (result or event ~= 'master.describe'))) then return result end
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
-- there: empty), a new game empties it. The game's load / new game command is seen by src/gear/savegames.cpp.
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
        native.clear_pets()
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

-- The kills this client sent (src/gear/netmsg.cpp) as the event 'kill' (ev): killer (a player, nil if a creature killed
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

-- Chat lines (gear.chat, src/gear/chat.cpp): every line the game adds to its message list (chat, server and game
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
