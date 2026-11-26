-- A test can show any frame of an animation by moving the clock there (sbx.seek): a box fading from opacity 1 to 0.2 in 2 s (linear) is
-- drawn at 0, 0.5, 1, 1.5 and 1.875 s after its start and its pixel must be the blend for that point; at 2 s the animation is over
-- and the box is opaque again (RmlUi keeps no end state). (sbx.set_time alone would not do: RmlUi
-- advances an animation by at most 0.1 s per update.)
--   gear_sandbox --hidden --no-mods --mods tools\sandbox\fuzz\mods --script tools\sandbox\tests\clock_animation.lua
local doc = [[<rml><head><style>
@keyframes fade { from { opacity: 1; } to { opacity: 0.2; } }
body { width: 100%; height: 100%; }
#box { position: absolute; left: 100px; top: 100px; width: 80px; height: 80px; background-color: #ff0000; animation: 2s linear 1 fade; }
</style></head><body><div id="box"></div></body></rml>]]
local id = assert(rml.rml_load_string('fuzz', doc, false))
local function show()
    rml.rml_update(id, 0, 0, false)
    rml.rml_draw(id, 1, -1)
    sbx.commit()
    sbx.draw()
    return sbx.pixel(140, 140)
end
local t0 = sbx.time()
show()                                  -- the animation starts with the first update
local bg = 0x202830                     -- the sandbox's clear colour
local fails = 0
for _, t in ipairs({0, 0.5, 1, 1.5, 1.875, 2}) do
    sbx.seek(t0 + t, function() rml.rml_update(id, 0, 0, false) end)
    local px = show()
    local opacity = t < 2 and 1 - 0.8 * (t / 2) or 1
    local want = math.floor(255 * opacity + (bg / 65536 % 256) * (1 - opacity) + 0.5)
    local red = math.floor(px / 65536) % 256
    local ok = math.abs(red - want) <= 2
    if not ok then fails = fails + 1 end
    sbx.log(('t = %.2f s: red %d, expected %d%s'):format(t, red, want, ok and '' or '  <-- wrong'))
end
return fails
