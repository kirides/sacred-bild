-- Twitch Integration (base mod), the Sacred counterpart of kirides' Gothic II and Risen Twitch integrations.
--
-- The connector program hosts \\.\pipe\__TwitchIntegration_Kirides_Conn and writes JSON messages (Sacred Gear's
-- pipes read the 2-byte little-endian length in front):
--     {"type":"redemption","data":{"title":"Spawn Monster","redeemer":"alice"}}
--     {"type":"streamelements-perk","data":{"title":"Gold","redeemer":"alice"}}
--     {"type":"bits","data":{"bitsUsed":100,"user":"alice"}}
--     {"type":"chat","data":{"text":"#raid","sender":"alice"}}
--     {"type":"ping"}
-- custom.lua maps each one to actions; each action becomes an event offered to the action mods:
--     gear.emit('twitch.command', ctx)
-- ctx = {type, user, command, args (string), argv (list), handled = false, defer = false, message = nil}. An action
-- mod that knows ctx.command sets ctx.handled = true, or ctx.defer = true for "not now". Events run only while a game
-- runs and the hero is alive; earlier ones wait (and expire after config.max_age_s).

local cfg = gear.config
local TYPES = {REWARD_ADD = true, BITS_USED = true, CHAT = true}

local inbox = {}        -- {line, t}
local deferred = {}     -- {ev, t}
local last_run = -1e9
local stats = {received = 0, mapped = 0, unmapped = 0, executed = 0, ignored = 0, expired = 0, bad = 0}
local cooldowns = {}

local custom = {rewards = {}, perks = {}, bits = {}, chat = {}}
do
    local ok, loaded = pcall(require, 'custom')
    if ok and type(loaded) == 'table' then
        for k in pairs(custom) do custom[k] = type(loaded[k]) == 'table' and loaded[k] or {} end
    else
        print('custom.lua could not be loaded: ' .. tostring(loaded))
    end
end

local now = gear.time

local function split(s)
    local t = {}
    for w in s:gmatch('%S+') do t[#t + 1] = w end
    return t
end

local function parse(line)
    local typ, user, command, rest = line:match('^(%S+)%s+(%S+)%s+(%S+)%s*(.-)%s*$')
    if not typ or not TYPES[typ] then return nil end
    return {type = typ, user = user, command = command, args = rest, argv = split(rest)}
end

local function actions_of(entry)
    if type(entry) ~= 'table' then return nil end
    if entry.actions then return entry.actions, tonumber(entry.cooldown_s) or 0 end
    return entry, 0
end

local function lookup(tbl, key)
    local want = tostring(key):upper()
    for k, v in pairs(tbl) do
        if type(k) == 'string' and k:upper() == want then return v end
    end
end

local function translate(msg)
    local typ, d = msg.type, type(msg.data) == 'table' and msg.data or {}
    local out = {}
    local function add(kind, user, entry, cooldown_key)
        local list, cooldown = actions_of(entry)
        if not list then return end
        if cooldown > 0 and cooldown_key then
            local t = now()
            if cooldowns[cooldown_key] and t - cooldowns[cooldown_key] < cooldown then return end
            cooldowns[cooldown_key] = t
        end
        for _, a in ipairs(list) do
            if type(a) == 'string' and a:match('%S') then
                out[#out + 1] = ('%s %s %s'):format(kind, user, a:match('^%s*(.-)%s*$'))
            end
        end
    end
    local user = tostring(d.redeemer or d.user or d.sender or ''):gsub('%s', '')
    if user == '' then user = 'someone' end
    if typ == 'redemption' then add('REWARD_ADD', user, lookup(custom.rewards, d.title or ''))
    elseif typ == 'streamelements-perk' then add('REWARD_ADD', user, lookup(custom.perks, d.title or ''))
    elseif typ == 'bits' then add('BITS_USED', user, custom.bits[tonumber(d.bitsUsed) or -1])
    elseif typ == 'chat' then add('CHAT', user, custom.chat[tostring(d.text or '')], 'chat:' .. tostring(d.text))
    end
    return out
end

local function ready()
    local hero = gear.world.hero()
    return hero ~= nil and not hero:is_dead() and gear.world.can_change()
end

local function execute(ev)
    local ctx = {type = ev.type, user = ev.user, command = ev.command, args = ev.args, argv = ev.argv,
                 handled = false, defer = false}
    gear.emit('twitch.command', ctx)
    if ctx.defer then return 'defer' end
    if ctx.handled then
        print(ctx.message or (ev.user .. ' -> ' .. ev.command .. ' ' .. ev.args))
        return 'done'
    end
    return 'ignored'
end

local function handle(ev, result, entry)
    if result == 'done' then
        stats.executed = stats.executed + 1
    elseif result == 'defer' then
        if entry then deferred[#deferred + 1] = entry
        elseif #deferred < cfg.max_deferred then deferred[#deferred + 1] = {ev = ev, t = now()} end
    else
        stats.ignored = stats.ignored + 1
        print(("nobody handles '%s' (from %s)"):format(ev.command, ev.user))
    end
end

local function on_message(text)
    local msg = gear.json.decode(text)
    if type(msg) ~= 'table' or type(msg.type) ~= 'string' then
        stats.bad = stats.bad + 1
        return
    end
    if msg.type == 'ping' then return end
    stats.received = stats.received + 1
    local lines = translate(msg)
    if #lines == 0 then stats.unmapped = stats.unmapped + 1 return end
    for _, line in ipairs(lines) do
        stats.mapped = stats.mapped + 1
        if #inbox < 200 then inbox[#inbox + 1] = {line = line, t = now()} end
    end
end

local function step()
    local t = now()
    if cfg.max_age_s > 0 then
        while inbox[1] and t - inbox[1].t > cfg.max_age_s and not ready() do
            table.remove(inbox, 1)
            stats.expired = stats.expired + 1
        end
    end
    if t - last_run < cfg.interval_s or not ready() then return end
    last_run = t
    for i = #deferred, 1, -1 do
        if t - deferred[i].t > cfg.defer_s then table.remove(deferred, i); stats.expired = stats.expired + 1 end
    end
    if deferred[1] then
        local entry = table.remove(deferred, 1)
        handle(entry.ev, execute(entry.ev), entry)
        return
    end
    local item = table.remove(inbox, 1)
    if not item then return end
    local ev = parse(item.line)
    if not ev then stats.bad = stats.bad + 1 return end
    handle(ev, execute(ev))
end

gear.on('frame', step)

local pipe = gear.pipe.open('__TwitchIntegration_Kirides_Conn')
pipe.on_message = on_message
pipe.on_connect = function() print('connector connected') end
pipe.on_disconnect = function() print('connector gone') end

-- Test hooks: feed a raw action line or a connector message as if the connector had sent it; read the counters.
gear.on('twitch.inject', function(line) inbox[#inbox + 1] = {line = line, t = now()} end)
gear.on('twitch.inject_message', on_message)
gear.on('twitch.status', function(out)
    out.connected = pipe:connected()
    out.inbox, out.deferred = #inbox, #deferred
    for k, v in pairs(stats) do out[k] = v end
end)
