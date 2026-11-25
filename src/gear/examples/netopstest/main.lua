-- Custom net messages (gear.net) against a real game server in a LAN game this client hosts.
--   Sacred.exe -gear-auto -gear-mods=netopstest -gear-instance=NAME      (config expect = 'stock' or 'remake')
-- 'remake': GameServer.exe is the SacredEngineRemake server (tools/test/gs-launcher) with scripts\net_echo.lua in the game folder.
local mp, net = gear.mp, gear.net
if not gear.auto then return end
-- chat_only: decline the server's custom message types (restrict them to numbers it will not offer): channels stay on chat
if gear.config.chat_only then net.configure({control = 0x2F0, data = 0x2F1}) end
local A = gear.auto
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

local caps = {}   -- the handshake can finish before the game is up, so this listens from the start
gear.on('net_caps', function(c) caps[#caps + 1] = c end)
local function finish()
    print(('netopstest: %d passed, %d failed'):format(passed, failed))
    A.write('netopstest.json', gear.json.encode({results = results, passed = passed, failed = failed}))
    A.quit(failed == 0 and 0 or 1)
end

-- the chat carrier next to the custom messages
local function chat_carrier()
    mp.force_network(true)
    local got = {}
    gear.on('mp_message', function(from, ch, p) if ch == 'netopstest' then got[#got + 1] = p end end)
    check('chat carrier send', mp.send('netopstest', {n = 1}, mp.local_slot()) == true)
    check('chat carrier delivers', gear.wait_until(function() return #got > 0 end, 15))
    mp.force_network(false)
end

gear.task(function()
    host_lan()
    if not A.wait_for_game(240) then
        A.screenshot('lan-no-game')
        check('game', false)
        A.write('netopstest.json', gear.json.encode({results = results}))
        return A.quit(1)
    end
    gear.wait(3)
    local expect = gear.config.expect or 'stock'
    check('net module', net ~= nil)
    if expect == 'stock' then
        gear.wait(6)
        check('no answer from the original server: not supported', net.supported() == false and #caps == 0)
        check('limits are nil, the server is not reachable over chat', net.limits() == nil and net.transport('server') == nil)
        local ch = net.channel('example:echo')
        check('send is refused with a reason', select(2, ch:send('x', 'server')) ~= nil)
        check('peer_supported is false', net.peer_supported(mp.local_slot()) == false)
        chat_carrier()
        A.screenshot('netopstest-stock')
        return finish()
    end

    local chat_only = gear.config.chat_only == true
    if chat_only then
        check('the server queried over chat, the client declined: channels use chat', gear.wait_until(function() return net.transport('server') == 'chat' end, 30)
            and net.supported() == false, net.transport('server'))
        check('net_caps tells about chat', #caps >= 1 and caps[#caps].chat == true and caps[#caps].supported == false)
    else
        check('handshake with the remake server (chat query, answer, HELLO)', gear.wait_until(function() return net.supported() end, 30))
        local lim = net.limits()
        check('limits announced', lim and lim.max_fragment == 4000 and lim.max_message == 262144 and lim.bytes_per_second == 65536 and lim.in_flight == 8,
            lim and ('%d/%d/%d/%d'):format(lim.max_fragment, lim.max_message, lim.bytes_per_second, lim.in_flight))
        check('net_caps fired', #caps >= 1 and caps[#caps].supported == true and caps[#caps].chat == true)
        check('this computer is in the peer list', gear.wait_until(function() return net.peer_supported(mp.local_slot()) end, 5), mp.local_slot())
        check('transport to the server is custom', net.transport('server') == 'custom')
    end

    local ch = net.channel('example:echo')
    local got = {}
    ch:on(function(from, payload) got[#got + 1] = {from = from, payload = payload} end)
    local function await(n, timeout) return gear.wait_until(function() return #got >= n end, timeout or 15) end
    local me = mp.local_player()

    check('send a table', ch:send({text = 'hello', n = 7}, 'server') == true)
    check('table echoed with the sender stamped by the server', await(1) and got[1].payload.text == 'hello' and got[1].payload.n == 7
        and got[1].payload.from == me.name and got[1].from.slot == 0, got[1] and gear.json.encode(got[1].payload))
    check('send a string', ch:send('abc\0def', 'server') == true)
    check('string echoed upper case, bytes intact', await(2) and got[2].payload == 'ABC\0DEF', got[2] and #got[2].payload)

    local big = {}
    local size = chat_only and 40000 or 100000
    for i = 1, size do big[i] = string.char(97 + i % 26) end
    big = table.concat(big)
    local t0 = os.clock()
    check('send ' .. size .. ' bytes', ch:send(big, 'server') == true)
    check(size .. ' bytes echoed (paced out, reassembled back)', await(3, 60) and got[3].payload == big:upper(), got[3] and #got[3].payload)
    check('a message to the local slot is refused', select(2, ch:send('x', mp.local_slot())) ~= nil)

    local unknown = net.channel('example:nobody')
    check('send on a channel the server does not know', unknown:send('x', 'server') == true)
    check('the transport did not change', net.transport('server') == (chat_only and 'chat' or 'custom'))
    gear.wait(2)
    check('the connection survived it', not gear.world.hero():is_dead() and net.transport('server') ~= nil)
    chat_carrier()
    A.screenshot('netopstest-remake')
    finish()
end)
