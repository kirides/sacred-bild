-- Stage 1 spike: custom opcodes against the stock GameServer.exe (LAN host).
--   Sacred.exe -gear-auto -gear-mods=netopsspike -gear-instance=NAME
local mp = gear.mp
if not gear.auto then return end
local A = gear.auto
local native = gear._native_net
local passed, failed, results = 0, 0, {}
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    local line = ('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or '')
    results[#results + 1] = line
    print(line)
end
local function host_lan()
    A.wait_for_menu(120)
    gear.wait(8)
    A.screenshot('lan-0-menu')
    A.ui_click(512, 352)                    -- Multiplayer
    gear.wait(2)
    A.ui_click(512, 352 - 38 / 1.1719)      -- LAN
    gear.wait(3)
    local function px(x, y) return (x - 200) / 1.1719, y / 1.1719 end
    A.ui_click(px(gear.config.hero_x, gear.config.hero_y))
    gear.wait(2)
    A.screenshot('lan-1-heroes')
    A.ui_click(px(910, 838))                -- next
    gear.wait(3)
    A.screenshot('lan-2-games')
    A.ui_click(px(553, 448))                -- create
    gear.wait(3)
    A.ui_click(px(747, 448))                -- create: the game starts its gameserver and connects to it
    -- the dialog turns into "join game" with Starten at the same place once the server is up: press it until in game
    for _ = 1, 8 do
        gear.wait(5)
        if A.wait_for_game(1) then return end
        A.ui_click(px(747, 448))            -- Starten
        if A.wait_for_game(10) then return end
    end
end

gear.task(function()
    host_lan()
    if not A.wait_for_game(240) then
        A.screenshot('lan-no-game')
        check('game', false)
        A.write('netopsspike.json', gear.json.encode({results = results}))
        return A.quit(1)
    end
    gear.wait(3)
    local c, d, ready = native.net_ops()
    print(('spike: ops control 0x%X data 0x%X ready %s'):format(c, d, tostring(ready)))
    check('ops ready in a network game', ready == true)
    local frame = '\1\0\0\0' .. 'abcdefgh'
    check('send 0x1E0', native.net_send_op(c, '\1\1\0\0\0\0\0\0\0'))
    check('send 0x1E1 small', native.net_send_op(d, frame))
    check('send 0x1E1 4016 bytes', native.net_send_op(d, string.rep('x', 4016)))
    check('refuses a stock id', native.net_send_op(0x3, 'abcd') == false)
    gear.wait(4)
    local t = native.net_take_op()
    check('nothing came back from the stock server', t == nil, t)
    local hero = gear.world.hero()
    check('hero alive after the sends', hero and not hero:is_dead())
    mp.force_network(true)
    local got = {}
    gear.on('mp_message', function(from, ch, p) if ch == 'netopsspike' then got[#got + 1] = p end end)
    check('chat carrier send after the custom ops', mp.send('netopsspike', {n = 1}, mp.local_slot()) == true)
    check('chat carrier still delivers', gear.wait_until(function() return #got > 0 end, 15))
    mp.force_network(false)
    A.screenshot('netopsspike-end')
    print(('spike: %d passed, %d failed'):format(passed, failed))
    A.write('netopsspike.json', gear.json.encode({results = results, passed = passed, failed = failed}))
    A.quit(failed == 0 and 0 or 1)
end)

