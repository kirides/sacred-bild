-- A pinpointed self test of gear.mp's identity calls and the mod channel.
--   single player: Sacred.exe -gear-auto -gear-load=1 -gear-mods=mptest -gear-instance=NAME
--   LAN host:      Sacred.exe -gear-auto -gear-mods=mptest -gear-instance=NAME   (config mode = 'lan' in gear_mods.json)
-- Results: PASS/FAIL lines in SacredGear.log and GearAutomation\NAME\mptest.json.
local mp = gear.mp
if not gear.auto then return end
local A = gear.auto
local passed, failed, results = 0, 0, {}
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    local line = ('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail ~= nil and (': ' .. tostring(detail)) or '')
    results[#results + 1] = line
    print(line)
end

local got = {}
gear.on('mp_message', function(from, channel, payload)
    if channel == 'mptest' then got[#got + 1] = {from = from, payload = payload, at = os.clock()} end
end)

local function big_table()
    local parts = {}
    for i = 1, 5000 do parts[i] = string.char(33 + (i * 31) % 90) end
    return {text = table.concat(parts), n = 5000, list = {1, 2, 3}, nested = {ok = true}}
end

local function same(a, b)
    return type(a) == 'table' and type(b) == 'table' and a.text == b.text and a.n == b.n and #a.list == 3
        and a.list[3] == 3 and a.nested and a.nested.ok == true
end

local function send_and_wait(to, label, timeout)
    got = {}
    local msg = big_table()
    local ok, err = mp.send('mptest', msg, to)
    check(label .. ': send', ok == true, err)
    if not ok then return end
    local done = gear.wait_until(function() return #got > 0 end, timeout)
    check(label .. ': arrived', done and #got == 1, #got)
    if got[1] then
        check(label .. ': intact (5000 bytes and the table around them)', same(got[1].payload, msg))
        check(label .. ': from the local player', got[1].from.is_local == true and got[1].from.slot == mp.local_slot(), got[1].from.slot)
    end
end

local function host_lan()
    A.wait_for_menu(120)
    gear.wait(8)
    A.ui_click(512, 352)                    -- Multiplayer
    gear.wait(2)
    A.ui_click(512, 352 - 38 / 1.1719)      -- LAN
    gear.wait(3)
    local function px(x, y) return (x - 200) / 1.1719, y / 1.1719 end
    A.ui_click(px(gear.config.hero_x, gear.config.hero_y))
    gear.wait(2)
    A.ui_click(px(910, 838))                -- next
    gear.wait(3)
    A.ui_click(px(553, 448))                -- create
    gear.wait(3)
    A.ui_click(px(747, 448))                -- create: the game starts its gameserver and connects to it
end

gear.task(function()
    local lan = gear.config.mode == 'lan'
    if lan then host_lan() end
    if not A.wait_for_game(lan and 240 or 180) then
        check('game', false)
        A.write('mptest.json', gear.json.encode({results = results}))
        return A.quit(1)
    end
    gear.wait(3)
    local me = mp.local_player()
    print('mptest: local player ' .. tostring(me and me.name) .. ' key ' .. tostring(me and me.key) .. ' slot '
        .. tostring(mp.local_slot()) .. ' host ' .. tostring(mp.host_slot()) .. ' is_host ' .. tostring(mp.is_host()))
    check('local player', me ~= nil)
    check('hero name', me and type(me.name) == 'string' and me.name ~= '' and not me.name:match('^hero %d+$'), me and me.name)
    check('key', me and me.key == me.name:lower() .. ':' .. tostring(me.class), me and me.key)
    check('is_host', mp.is_host() == true and me and me.is_host == true)
    check('host slot', mp.host_slot() == mp.local_slot(), mp.host_slot())
    local hero = gear.world.hero()
    check('Creature:name', hero and hero:name() == (me and me.name), hero and hero:name())
    -- the loopback (single player, or the host sending to itself)
    send_and_wait('host', 'loopback', 5)
    if lan then
        -- the network path: to the local slot through the real chat carrier, out to the server and back
        mp.force_network(true)
        local stats = mp._channel
        local before = stats.received or 0
        send_and_wait(mp.local_slot(), 'chat carrier', 20)
        check('chat carrier: frames came back through the network', (stats.received or 0) - before >= 22, (stats.received or 0) - before)
        mp.force_network(false)
        gear.wait(1)
        A.screenshot('mptest-chat')         -- the chat box must show none of it
    end
    print(('mptest: %d passed, %d failed'):format(passed, failed))
    A.write('mptest.json', gear.json.encode({results = results, passed = passed, failed = failed,
        name = me and me.name, key = me and me.key, slot = mp.local_slot(), host = mp.host_slot()}))
    A.quit(failed == 0 and 0 or 1)
end)
