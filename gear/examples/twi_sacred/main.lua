-- TWI_* actions for the Twitch Integration base mod (event twitch.command), the Sacred counterpart of TWI_Kirides:
-- monsters spawned just out of sight and labelled with the viewer's name, gold given or taken, hit points.
--
--   actions.<lower-case command> = function(ctx, argv, hero) return true | false [, why] end
-- true: done. false without why: "not now" (offered again later). false, why: bad arguments (dropped, logged).

local cfg = gear.config
local W = gear.world
local actions = {}

local function num(s, default)
    local n = tonumber(s)
    if n == nil or n ~= n or n == math.huge or n == -math.huge then return default end
    return n
end
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function choice(list) return list[math.random(#list)] end

-- The monster pool: type ids of the configured names, found once the game's types are loaded.
local pool
local function monsters()
    if pool then return pool end
    pool = {}
    for _, name in ipairs(cfg.monsters or {}) do
        local id = gear.data.object_types:find(name)
        if id then pool[#pool + 1] = {id = id, name = name} else print('no object type ' .. name) end
    end
    return pool
end

local current_user = ''

-- `n` creatures of type `kind` (id or name) around the hero, labelled "<user>'s <name>".
local function spawn_around(hero, kind, name, n)
    local x, y, layer = hero:position()
    local made = 0
    for _ = 1, n do
        local sx, sy = W.around(x, y, cfg.spawn_min, cfg.spawn_max)
        local label = current_user ~= '' and (current_user .. "'s " .. name) or nil
        local c, why = W.spawn(kind, sx, sy, {layer = layer, label = label})
        if c then made = made + 1 else print('spawn failed: ' .. tostring(why)) end
    end
    return made
end

actions.twi_spawnenemy = function(_, argv, hero)
    if not argv[1] then return false, 'usage: TWI_SpawnEnemy <type name> [amount]' end
    if not gear.data.object_types:find(argv[1]) then return false, 'no object type ' .. argv[1] end
    return spawn_around(hero, argv[1], argv[1], clamp(math.floor(num(argv[2], 1)), 1, 10)) > 0
end

actions.twi_spawnrandommonster = function(_, argv, hero)
    local list = monsters()
    if #list == 0 then return false, 'no monster types configured (config.monsters)' end
    local made = 0
    for _ = 1, clamp(math.floor(num(argv[1], 1)), 1, 10) do
        local m = choice(list)
        made = made + spawn_around(hero, m.id, m.name, 1)
    end
    return made > 0
end

-- TWI_Raid [n]: a group of one kind (n, default 2..4)
actions.twi_raid = function(_, argv, hero)
    local list = monsters()
    if #list == 0 then return false, 'no monster types configured (config.monsters)' end
    local m = choice(list)
    return spawn_around(hero, m.id, m.name, clamp(math.floor(num(argv[1], math.random(2, 4))), 1, 12)) > 0
end

-- TWI_Money 100 gives gold, TWI_Money -50 takes it (never more than the hero has).
actions.twi_money = function(_, argv, hero)
    local n = math.floor(num(argv[1], 0))
    if n == 0 then return false, 'usage: TWI_Money <amount>' end
    return hero:add_gold(clamp(n, -1e7, 1e7)) == true
end

actions.twi_fullheal = function(_, _, hero) return hero:set_hp(hero:max_hp()) == true end

actions.twi_sethp = function(_, argv, hero)
    return hero:set_hp(clamp(math.floor(num(argv[1], 1)), 1, hero:max_hp())) == true
end

-- TWI_SetHP_Timed <hp> <seconds>: hit points held at most at <hp> for the time.
actions.twi_sethp_timed = function(_, argv)
    local n, s = math.floor(num(argv[1], 1)), clamp(num(argv[2], 5), 1, 120)
    local until_t = gear.time() + s
    local h
    h = gear.on('frame', function()
        local hero = W.hero()
        if gear.time() > until_t or not hero then gear.off(h) return end
        if hero:hp() > n then hero:set_hp(n) end
    end)
    return true
end

-- TWI_RandomTeleport [min] [max]: somewhere 10..30 tiles away (the engine refuses spots it cannot place the hero)
actions.twi_randomteleport = function(_, argv, hero)
    local x, y = hero:position()
    local min = num(argv[1], 10) * W.TILE
    local max = num(argv[2], 30) * W.TILE
    for _ = 1, 8 do
        local tx, ty = W.around(x, y, min, max)
        if hero:teleport(tx, ty) then return true end
    end
    return false
end

gear.on('twitch.command', function(ctx)
    local fn = actions[ctx.command:lower()]
    if not fn then return end
    local hero = W.hero()
    if not hero then ctx.defer = true return end
    current_user = ctx.user or ''
    local ok, res, why = pcall(fn, ctx, ctx.argv, hero)
    current_user = ''
    if not ok then
        print(('%s failed: %s'):format(ctx.command, tostring(res)))
        ctx.handled = true
    elseif res then
        ctx.handled = true
    elseif why then
        print(('%s: %s'):format(ctx.command, why))
        ctx.handled = true
    else
        ctx.defer = true
    end
end)

gear.on('session.start', function() pool = nil end)
