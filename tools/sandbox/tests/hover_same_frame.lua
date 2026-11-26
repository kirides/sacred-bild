-- An element that moves under a still mouse gets its :hover in the same frame (no one-frame lag). Exit code 0: it does.
--   gear_sandbox --hidden --no-mods --mods tools\sandbox\fuzz\mods --script tools\sandbox\tests\hover_same_frame.lua
local doc = [[<rml><head><style>
body { width: 100%; height: 100%; }
#b { position: absolute; left: 0px; top: 0px; width: 100px; height: 40px; background-color: #404040; }
#b:hover { width: 160px; }
</style></head><body><div id="b"></div></body></rml>]]
local id = assert(rml.rml_load_string('fuzz', doc, false))
local function frame()
    rml.rml_update(id, 300, 20, true)
    rml.rml_draw(id, 1, -1)
    sbx.commit()
    sbx.advance(1 / 60)
end
frame(); frame()
local w0 = rml.rml_rect(id, '#b').w
rml.rml_style(id, '#b', 'left', '250px')       -- now under the still mouse at (300, 20)
frame()
local w1 = rml.rml_rect(id, '#b').w
frame()
local w2 = rml.rml_rect(id, '#b').w
sbx.log(('width before %.0f, the frame of the move %.0f, one frame later %.0f'):format(w0, w1, w2))
return (w1 == 160 and w2 == 160) and 0 or 1
