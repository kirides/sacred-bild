-- gear.store: a mod's own data kept in JSON files in its folder (a log, totals, saved lists). Shipped inside Sacred Gear and run
-- once per mod, in the mod's own environment (so `gear` below is the mod's). It only uses the public API (gear.read, gear.save,
-- gear.json, gear.every, gear.on).

local S = {}
local warned = {}

local function warn(name, what)
    if warned[name] then return end
    warned[name] = true
    gear.log(('gear.store: %s: %s'):format(name, tostring(what)))
end

-- The decoded content of the file `name`, or `default` when the file is missing, is not JSON, or (with a table as default)
-- does not hold a table. The second result says why the default was taken: 'missing', 'invalid' or nil.
function S.load(name, default)
    local text = gear.read(name)
    if text == nil or text == '' then return default, 'missing' end
    local ok, value = pcall(gear.json.decode, text)
    if not ok or value == nil or (type(default) == 'table' and type(value) ~= 'table') then
        warn(name, 'not readable, the defaults are used')
        return default, 'invalid'
    end
    return value, nil
end

-- Writes `value` as JSON to the file `name`. Returns true, or false and the error (the first error of a file is also logged).
function S.save(name, value)
    local ok, text = pcall(gear.json.encode, value)
    if not ok then warn(name, text) return false, text end
    local saved, err = gear.save(name, text)
    if not saved then warn(name, err) return false, err end
    return true
end

-- A file that is loaded once and saved when it changed: handle.data is the table; call handle:changed() after changing it.
-- opts:
--   every    seconds between saves of a changed file (default 30; 0: only on unload and handle:save())
--   check    function(data) -> data: repairs or replaces what was loaded (called once, after loading)
--   value    function() -> the table to write (default handle.data), for mods that keep their state elsewhere
-- The file is also saved when the mod unloads (the game closes, the mod is switched off), if it changed.
function S.file(name, default, opts)
    opts = opts or {}
    local h = {name = name, dirty = false}
    local data = S.load(name, default)
    if opts.check then data = opts.check(data) end
    if data == nil then data = default end
    h.data = data
    function h:changed() self.dirty = true end
    function h:save()
        self.dirty = false
        local value = self.data
        if opts.value then value = opts.value() end
        return S.save(self.name, value)
    end
    function h:flush()
        if self.dirty then return self:save() end
        return true
    end
    local every = tonumber(opts.every) or 30
    if every > 0 then gear.every(every, function() h:flush() end) end
    gear.on('unload', function() h:flush() end)
    return h
end

local function copy(v)
    if type(v) ~= 'table' then return v end
    local out = {}
    for k, x in pairs(v) do out[k] = copy(x) end
    return out
end

-- A file of the savegame being played: what the mod keeps about this character / game (totals, progress, a log), not
-- settings or things shared by all games. The same handle as `file` (handle.data, changed(), save(), flush()), but the
-- data belongs to the savegame: a save into a slot keeps it for that slot, loading a slot brings back what was kept
-- for it, a new game starts with `default` again (see gear.savegame). Progress since the last save is gone after a load,
-- as the game's own progress is. handle.data is replaced then: always reach the data through the handle.
-- opts as for `file`, and:
--   on_change  function(handle, kind, scope): the handle holds the data of another game now (kind 'load', 'hero' or
--              'new'; the handler of 'savegame.changed' of the mod runs after it)
--   migrate    true: a file `name` of the mod's folder from before the mod used per game data is taken once, by the first
--              loaded savegame that has no data yet (it is what the mod collected so far, so it goes to the game that is
--              continued); a marker file <name>.migrated.txt records that
function S.game(name, default, opts)
    opts = opts or {}
    local h = {name = name, dirty = false, per_game = true}
    local marker = (name:gsub('%.json$', '')) .. '.migrated.txt'
    local function load()
        local text = gear.savegame.read(name)
        local data
        if text and text ~= '' then
            local ok, value = pcall(gear.json.decode, text)
            if ok and value ~= nil and not (type(default) == 'table' and type(value) ~= 'table') then data = value
            else warn(name, 'not readable, the defaults are used') end
        elseif opts.migrate and (gear.savegame.kind() == 'load' or gear.savegame.kind() == 'hero')
            and gear.read(marker) == nil then
            local old = S.load(name, nil)
            if old ~= nil then
                gear.save(marker, tostring(gear.savegame.scope()))
                data = old
                h.dirty = true
            end
        end
        if opts.check then data = opts.check(data == nil and copy(default) or data) end
        if data == nil then data = copy(default) end
        h.data = data
    end
    load()
    function h:changed() self.dirty = true end
    function h:save()
        self.dirty = false
        local value = self.data
        if opts.value then value = opts.value() end
        local ok, text = pcall(gear.json.encode, value)
        if not ok then warn(self.name, text) return false, text end
        local saved, err = gear.savegame.write(self.name, text)
        if not saved then warn(self.name, err) return false, err end
        return true
    end
    function h:flush()
        if self.dirty then return self:save() end
        return true
    end
    local every = tonumber(opts.every) or 30
    if every > 0 then gear.every(every, function() h:flush() end) end
    gear.on('unload', function() h:flush() end)
    -- Before the working copy is kept for a savegame: write what changed. After the game changed: read what it holds.
    gear.on('savegame.saving', function() h:flush() end)
    gear.on('savegame.changed', function(kind, scope)
        h.dirty = false
        load()
        if opts.on_change then opts.on_change(h, kind, scope) end
        if h.dirty then h:flush() end
    end)
    if h.dirty then h:flush() end
    return h
end

return S
