-- An automated UI test against real mods (docs/Sandbox.md): the session tracker's window, its "New session" button hovered and
-- pressed, and F2 closing it again; a snapshot after each step. Run from the repository root with a copy of the mods:
--   gear_sandbox --hidden --mods <copy of sacred-gear-mods\src> --enable sacred-theme,sessiontracker --size 1600x900
--       --script tools\sandbox\examples\session_window.lua <out dir>
local out = arg[1] or '.'
local VK_F2 = 0x71

sbx.frame(30)                               -- the mods load and lay their documents out
sbx.snapshot(out .. '/1_open.png')

sbx.mouse(247, 523)                         -- over "New session" (the window's default place)
sbx.frame(10)
sbx.snapshot(out .. '/2_hover.png')

local taken = sbx.button(true)
sbx.frame(2)
sbx.button(false)
sbx.frame(10)
sbx.snapshot(out .. '/3_pressed.png')
sbx.log('the press went to a mod region: ' .. tostring(taken))

sbx.key(VK_F2, true)
sbx.frame(2)
sbx.key(VK_F2, false)
sbx.frame(10)
sbx.snapshot(out .. '/4_closed.png')
return taken and 0 or 1
