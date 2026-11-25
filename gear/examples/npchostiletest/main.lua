-- Self test (-gear-auto -gear-load=N -gear-mods=npchostiletest): a neutral NPC with a dialog turns hostile on the wrong answer and attacks the hero.
local A = gear.auto
local N = gear.npc
local results = {}
local function check(name, ok, detail)
    results[#results + 1] = {name = name, ok = ok and true or false, detail = detail}
    gear.log(('npchostiletest: %s %s %s'):format(ok and 'PASS' or 'FAIL', name, detail or ''))
end
local function finish()
    local failed = 0
    for _, r in ipairs(results) do if not r.ok then failed = failed + 1 end end
    A.write('result.json', gear.json.encode(results))
    A.quit(failed == 0 and 0 or 1)
end
local turned = 0
N.on('hostile', function() turned = turned + 1 end)

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
    local weapon = gear.shop.pick{category = gear.shop.CATEGORY.WEAPON, level = 40, quality = 8}
    check('a weapon type', weapon ~= nil)
    N.define('npchostiletest.boss', {
        type = model, name = 'Warden Brask', stance = 'neutral', talk_range = 400, level = 5,
        dialog = {start = 'hi', pages = {hi = {text = 'Move along, stranger. Or give me a reason.', options = {
            {id = 'polite', label = 'Farewell', close = true},
            {id = 'insult', label = 'Your mother was a goblin!', hostile = {level = 60, health = 6, damage = 3, weapon = weapon, tint = {parts = {[1] = 0xFF3030}}}},
        }}}},
    })
    local c = N.spawn('npchostiletest.boss', hx + 120, hy, {layer = layer})
    check('spawn', c ~= nil)
    if not c then return finish() end
    gear.wait(1)
    check('neutral: not an enemy of the hero', c:is_enemy(hero) == false and hero:is_enemy(c) == false, tostring(select(1, c:allegiance())))
    local before_hp = c:max_hp()
    check('not hostile yet', not N.is_hostile(c))
    A.screenshot('npchostile-1-neutral')
    N.talk(c)
    gear.wait(1)
    A.screenshot('npchostile-2-dialog')
    local x, y, w, h = A.widget('opt2', 'npchostiletest')
    check('the wrong option is drawn', x ~= nil)
    if x then A.ui_click(x + w / 2, y + h / 2) end
    gear.wait(1.5)
    check('the dialog ended', N.dialog_state() == nil)
    check('the NPC is hostile', N.is_hostile(c) and turned == 1)
    check('now an enemy of the hero', c:is_enemy(hero) == true, tostring(select(1, c:allegiance())))
    check('level raised', (c:max_hp() or 0) > before_hp * 3, ('%s -> %s'):format(tostring(before_hp), tostring(c:max_hp())))
    check('boss modifiers on', c:modifiers() ~= nil and c:modifiers().damage == 3)
    A.screenshot('npchostile-3-hostile')
    local hp0 = hero:hp()
    local fought = gear.wait_until(function() return hero:hp() < hp0 or c:action() == 'attacking' end, 25)
    check('it attacks the hero', fought, ('hero hp %s -> %s, npc %s'):format(tostring(hp0), tostring(hero:hp()), tostring(c:action())))
    A.screenshot('npchostile-4-fight')
    finish()
end)
