-- The theme mod's sprite sheet (mod.lua: theme_sprites) is linked only into documents that mention a `sprite-` class or load with sprites = true, and never
-- with sprites = false, for the screen-pixel (dp) documents too. The fixture theme paints .box blue and .sprite-1 red.
--   gear_sandbox --hidden --mods tools\sandbox\tests\theme_sprites\mods --enable spritetheme,spritetest --size 1600x900 --script tools\sandbox\tests\theme_sprites.lua [snapshot folder]
sbx.frame(30)
local RED, BLUE = 0xff0000, 0x0000ff
local cases = {
    {100, 'markup names the class: sheet linked', RED},
    {200, 'class set later from Lua, markup silent: no sheet', BLUE},
    {300, 'sprites = true: sheet linked', RED},
    {400, 'sprites = false: no sheet (the class has no rule)', nil},
}
local bad = 0
for row, dp in ipairs({false, true}) do
    local y = row == 1 and 100 or 300
    for _, c in ipairs(cases) do
        local got = sbx.pixel(c[1] + 10, y + 10)
        local want = c[3]
        local ok = (want == nil and got ~= RED and got ~= BLUE) or got == want
        if not ok then
            bad = bad + 1
            sbx.log(('FAIL %s %s: pixel %06x, want %s'):format(dp and 'dp' or 'px', c[2], got, want and ('%06x'):format(want) or 'neither'))
        end
    end
end
if arg[1] then sbx.snapshot(arg[1] .. '\\theme_sprites.png') end
sbx.log(bad == 0 and 'theme sprites: ok' or ('theme sprites: %d failures'):format(bad))
return bad

