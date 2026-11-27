-- Four documents (one 20 x 20 box each at x = 100, 200, 300, 400, and the same four laid out in dp at y = 300), see tests/theme_sprites.lua for what each must show.
local docs = {}
local function box(x, y, class)
    return ('<rml><body><div id="x" class="%s" style="position: absolute; left: %dpx; top: %dpx;"></div></body></rml>'):format(class, x, y)
end
local function open(name, markup, opts, late)
    local d = assert(gear.ui.rml.load_string(markup, opts))
    if late then d:class('#x', late, true) end
    docs[#docs + 1] = d
end
local function build()
for row, dp in ipairs({false, true}) do
    local y = row == 1 and 100 or 300
    open('mentions', box(100, y, 'sprite-1'), {dp = dp})                                   -- the markup names the class: the sheet is linked
    open('plain', box(200, y, 'box'), {dp = dp}, 'sprite-1')                               -- the class comes later from Lua and the markup does not name it: no sheet
    open('forced', box(300, y, 'box'), {dp = dp, sprites = true}, 'sprite-1')              -- sprites = true links it anyway
    open('refused', box(400, y, 'sprite-1'), {dp = dp, sprites = false})                   -- sprites = false never links it
end
end
gear.on('draw', function()
    if #docs == 0 then build() end
    for _, d in ipairs(docs) do d:draw({layer = 'top'}) end
end)

