-- Self test of the gameserver's file overlay: starts GameServer.exe through the game's own launch path, lets it run
-- gear.config.seconds, ends it and quits. The server's logs (SacredGear-server.log) show what it opened.
-- Run: Sacred.exe -gear-auto -gear-mods=servertest -gear-instance=NAME (tools\server-overlay-test.ps1 does it).
local A = gear.auto
gear.task(function()
    A.wait_for_menu(120)
    local pid = A.start_gameserver(gear.config.seconds)
    A.write('servertest.json', gear.json.encode({pid = pid}))
    A.quit(pid ~= 0 and 0 or 1)
end)
