-- The text caret blinks on exact frames, whatever the update cadence: a document updated only when needed (retained) and its twin updated
-- every frame (rml_nocache) show the caret in the same frames, through 400 frames of irregular steps, and it toggles every 0.7 s (RmlUi's blink time).
--   gear_sandbox --hidden --no-mods --mods tools\sandbox\fuzz\mods --script tools\sandbox\tests\clock_caret.lua
local doc = [[<rml><head><style>
body { width: 100%; height: 100%; font-family: "Sacred Game"; font-size: 18px; color: #ffffff; }
input.text { position: absolute; left: 20px; top: 20px; width: 200px; height: 24px; background-color: #303030; }
</style></head><body><input type="text" class="text" id="t" value="caret"/></body></rml>]]
local a = assert(rml.rml_load_string('fuzz', doc, false))
local b = assert(rml.rml_load_string('fuzz', doc, false))
rml.rml_nocache(b, true)
rml.rml_focus(a, '#t', true)
rml.rml_focus(b, '#t', true)
math.randomseed(7)
local bad, toggles, last = 0, {}, nil
local start = sbx.time()
for f = 1, 400 do
    -- irregular: mostly one frame, sometimes a few at once (a slow frame)
    sbx.advance(sbx.dt() * (math.random() < 0.8 and 1 or math.random(2, 5)))
    rml.rml_update(a, 0, 0, false)
    rml.rml_update(b, 0, 0, false)
    sbx.capture_begin(); rml.rml_draw(a, 1, -1); local ca = sbx.capture_end()
    sbx.capture_begin(); rml.rml_draw(b, 1, -1); local cb = sbx.capture_end()
    local ok, why = sbx.compare(ca, cb)
    if not ok then
        bad = bad + 1
        if bad <= 3 then sbx.log(('frame %d (t %.4f): %s'):format(f, sbx.time() - start, why)) end
    end
    local n = sbx.capture_info(cb)
    if last and n ~= last then toggles[#toggles + 1] = sbx.time() - start end
    last = n
    sbx.commit()
    sbx.capture_clear()
end
-- every toggle on a multiple of 0.7 s after the focus (the steps are coarse at times, so a toggle can be late by those frames only)
local off = 0
for _, t in ipairs(toggles) do
    local r = t % 0.7
    if r > 5 * sbx.dt() + 1e-9 then off = off + 1 end
end
sbx.log(('caret: %d frames differ between retained and twin, %d toggles, %d off the 0.7 s grid'):format(bad, #toggles, off))
return (bad == 0 and #toggles >= 12 and off == 0) and 0 or 1
