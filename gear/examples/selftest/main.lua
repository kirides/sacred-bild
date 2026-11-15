-- A self test of Sacred Gear and the game, for runs without a player:
--
--     Sacred.exe -gear-auto -gear-load=1 -gear-mods=selftest
--
-- loads savegame 1 (Save\GAME01.PAK) from the main menu, waits for the hero, checks what Gear reads and changes,
-- takes screenshots and writes GearAutomation\selftest.json (and types.txt, every object type: id, kind, name),
-- then quits with exit code 0 if every check passed, 1 if not. Without -gear-auto, gear.auto is nil and the mod
-- does nothing.

if not gear.auto then
    print('no automation (start the game with -gear-auto): nothing to do')
    return
end

local cfg = gear.config
local A = gear.auto
local results = {checks = {}, passed = 0, failed = 0}

local function check(name, ok, detail)
    results.checks[#results.checks + 1] = {name = name, ok = ok and true or false, detail = detail}
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function finish()
    results.state = A.state()
    A.write('selftest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end

local function dump_types()
    local types = gear.data.object_types
    local lines = {}
    for id = 1, types:count() - 1 do
        local name = types:name(id)
        if name then lines[#lines + 1] = ('%d\t%d\t%s'):format(id, types:kind(id) or -1, name) end
    end
    A.write('types.txt', table.concat(lines, '\n') .. '\n')
    return #lines
end

gear.task(function()
    local menu = A.wait_for_menu(cfg.timeout_menu)
    check('main menu', menu)
    if menu then
        A.screenshot('selftest-menu')
        local ui = A.ui()
        check('ui tree', #ui > 0, #ui .. ' visible controls')
    end

    -- -gear-load loads the savegame by itself; without it, a game has to be started some other way.
    local game = A.wait_for_game(cfg.timeout_game)
    check('game with a hero', game)
    if not game then return finish() end
    gear.wait(3)    -- let the world settle

    local hero = gear.world.hero()
    local x, y, layer = hero:position()
    check('hero position', x ~= nil, x and ('%d,%d layer %d'):format(x, y, layer))
    check('hero health', (hero:max_hp() or 0) > 0, ('%s / %s'):format(tostring(hero:hp()), tostring(hero:max_hp())))
    check('hero level', (hero:level() or 0) > 0, hero:level())
    check('world changes allowed', gear.world.can_change())

    local gold = hero:gold()
    check('gold readable', gold ~= nil, gold)
    if gold then
        hero:set_gold(gold + 123)
        check('gold write', hero:gold() == gold + 123, hero:gold())
        hero:set_gold(gold)
        check('gold restored', hero:gold() == gold)
    end

    local near = gear.world.creatures_near(x, y, 2000, {layer = layer})
    check('creatures near', #near > 0 and near[1] == hero, #near .. ' creatures')

    local count = dump_types()
    check('object types', count > 0, count .. ' named types')

    if cfg.spawn_type ~= '' then
        local sx, sy = gear.world.around(x, y, 380, 520)
        local c, why = gear.world.spawn(cfg.spawn_type, sx, sy, {label = 'selftest'})
        check('spawn ' .. cfg.spawn_type, c ~= nil, c and c.id or why)
        if c then
            gear.wait(1)
            check('spawned creature lives', c:exists() and c:is_creature(), c:type_name())
        end
    end

    A.screenshot('selftest-game')
    finish()
end)
