-- Sacred.exe -gear-auto -gear-mods=rmltest-charframe (no savegame): clicks into the hero selection, draws gear.ui.native.frame('thick') where the game's own
-- frame is, shifted up by 512 layout units (600 px exactly, so the pixels line up), and screenshots both for tools/ comparison scripts.
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local X, Y, W, H, DIV = 279.0, 538.5, 464, 214.3, 145
local show = false
gear.on('draw', function()
    if show then ui.native.frame(X, Y - 512, W, H, {style = 'medium', layer = 'top', dividers = {DIV}}) end
end)
gear.task(function()
    A.wait_for_menu(120); gear.wait(2)
    A.click(800, 338); gear.wait(3)
    A.screenshot('cf0')
    show = true; gear.wait(0.8)
    A.screenshot('cf')
    A.quit(0)
end)
