-- Self test: an NPC survives a savegame (-gear-auto -gear-load=N -gear-mods=npcsavetest). Saves into slot 90 of the game folder.
local A = gear.auto
local N = gear.npc
local SLOT = 90

local results = {}
local function check(name, ok, detail)
    results[#results + 1] = {name = name, ok = ok and true or false, detail = detail}
    gear.log(('npcsavetest: %s %s %s'):format(ok and 'PASS' or 'FAIL', name, detail or ''))
end

local changed, saved = 0, nil
gear.on('savegame.changed', function() changed = changed + 1 end)
gear.on('game.saved', function(slot) saved = slot end)

local function finish()
    local failed = 0
    for _, r in ipairs(results) do if not r.ok then failed = failed + 1 end end
    A.write('result.json', gear.json.encode(results))
    A.quit(failed == 0 and 0 or 1)
end

gear.task(function()
    A.wait_for_menu(120)
    A.wait_for_game(180)
    gear.wait(3)
    local hero = gear.world.hero()
    local hx, hy, layer = hero:position()
    local model
    for _, c in ipairs(gear.world.creatures_near(hx, hy, 900, {limit = 30})) do
        if c.id ~= hero.id then model = c:type() break end
    end
    N.define('npcsavetest.smith', {type = model, name = 'Smith Garran', tint = 0x40A0FF,
        dialog = {pages = {hi = {text = 'Hello again.', options = {{label = 'Bye', close = true}}}}}})
    local c = N.spawn('npcsavetest.smith', hx + 100, hy, {layer = layer, data = {note = 'kept'}})
    check('spawn', c ~= nil)
    if not c then return finish() end
    local id, type_ = c.id, c:type()
    gear.wait(1)
    print('save', gear.game.save(SLOT, 'npcsavetest'))
    gear.wait_until(function() return saved ~= nil end, 60)
    gear.wait(1)
    check('saved', saved == SLOT, tostring(saved))
    -- the NPC is removed, so the load has to bring it back
    c:remove()
    gear.wait(1)
    print('load', gear.game.load(SLOT))
    gear.wait_until(function() return changed >= 1 end, 120)
    gear.wait(6)
    local back = gear.world.object(id)
    check('the creature is back with the same id', back ~= nil and back:exists() and back:type() == type_)
    if back then
        check('it keeps its own name text', back:name_key() == 'npc:npcsavetest.smith', tostring(back:name_key()))
        check('display_name', back:display_name() == 'Smith Garran', tostring(back:display_name()))
        check('of() knows the kind again', N.of(back) == 'npcsavetest.smith')
        local list = N.list('npcsavetest.smith')
        check('the mod lists it again (per savegame data)', #list == 1 and list[1].data and list[1].data.note == 'kept', tostring(#list))
        gear.wait(2)
        A.screenshot('npcsavetest-after-load')
    end
    print('delete', gear.game.delete_save(SLOT))
    finish()
end)
