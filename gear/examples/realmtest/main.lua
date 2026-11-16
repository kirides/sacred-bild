-- Self test of gear.realm in the real game (the menus are driven with clicks; nothing is sent to the realm).
-- Run it with a given instance (the realm files are the instance's own, GearAutomation\<instance>\):
--   Sacred.exe -gear-auto -gear-instance=realm-1 -gear-mods=realmtest,realms
-- config.mode = 'internet' (default): chooses a realm, opens Multiplayer > Closed Internet and checks SacredGear.log for
-- "Realm: lobby is now ..."; 'lan': opens Multiplayer > LAN with a realm chosen (it must not be applied).
-- The result goes to GearAutomation\<instance>\realmtest.json.
local A = gear.auto
local results = {passed = 0, failed = 0, checks = {}}
local function check(name, cond, detail)
    local ok = cond and true or false
    results.passed = results.passed + (ok and 1 or 0)
    results.failed = results.failed + (ok and 0 or 1)
    results.checks[#results.checks + 1] = {name = name, ok = ok, detail = detail and tostring(detail) or nil}
    print(('%s: %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (' (' .. tostring(detail) .. ')') or ''))
end

local function find(id)
    for _, r in ipairs(gear.realm.list()) do
        if r.id == id then return r end
    end
end

gear.task(function()
    gear.wait(8)
    check('the default realm is first', gear.realm.list()[1].id == 'default' and gear.realm.current().id == 'default')
    check('add', gear.realm.add({id = 'testrealm', name = 'Test realm', host = 'realm.example.invalid', port = 7100}))
    check('two realms', #gear.realm.list() == 2 and find('testrealm') ~= nil)
    check('use', gear.realm.use('testrealm') == true and gear.realm.current().id == 'testrealm')
    -- Multiplayer (UI layout coordinates), then the sub menu's second entry (Closed Internet) or first (LAN).
    A.ui_click(512, 352)
    gear.wait(2)
    A.ui_click(512, gear.config.mode == 'lan' and 352 - 38 / 1.1719 or 352)
    gear.wait(3)
    A.screenshot('realm-menu')
    if gear.config.mode ~= 'lan' then
        -- The login screen: fake values in the four fields (nothing real is typed), then Einloggen. The game connects to
        -- the lobby host first, so the realm's host (which does not resolve) shows in the TinCat log.
        local fields = {{512, 183, 'realmtest'}, {512, 249, 'realmtest'}, {430, 319, 'AAAA'}, {430, 379, 'AAAA'}}
        for _, f in ipairs(fields) do
            A.ui_click(f[1], f[2])
            gear.wait(0.4)
            A.type(f[3])
            gear.wait(0.4)
        end
        A.screenshot('realm-login-filled')
        A.ui_click(512, 577)
        gear.wait(5)
        A.screenshot('realm-login-after')
    end
    A.write('realmtest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if gear.config.quit then A.quit(results.failed == 0 and 0 or 1) end
end)
