-- A window written as a document (ui/demo.rml and ui/demo.rcss) instead of drawn with gear.ui calls: the layout and the look
-- are HTML and CSS, the logic is Lua. F6 (or the Close button) shows and hides it. Everything a mod needs of gear.ui.rml:
--
--   load           the document, once (the files are in this mod's folder)
--   doc:on         a Lua function for an event of an element ('click', 'change' ...)
--   doc:text/class/style/value   change the document from Lua
--   doc:draw       every frame, from a draw handler: mouse, controller (all buttons are pad stops), keyboard, drawing

local ui = gear.ui

local doc, visible = nil, true
local clicks, boosted = 0, false

local function load()
    -- Data binding: the counter lives in the model; the document shows it with {{ clicks }} and calls back through data-event-click.
    local d, err = ui.rml.load('ui/demo.rml', {
        model = {clicks = 0},
        calls = {
            count = function() clicks = clicks + 1; doc.model.clicks = clicks end,
            reset = function() clicks = 0; doc.model.clicks = 0 end,
        },
    })
    if not d then
        print('rmldemo: ' .. tostring(err))
        return
    end
    doc = d

    d:on('#boost', 'click', function()
        boosted = not boosted
        d:class('#hp', 'boost', boosted)
        d:text('#boost', boosted and 'Highlight: on' or 'Highlight: off')
    end)
    d:on('#greet', 'click', function()
        local name = d:value('#name') or ''
        d:text('#greeting', name ~= '' and ('Hello, ' .. name .. '!') or 'Type a name first.')
    end)
    d:on('#close', 'click', function() visible = false; d:hide() end)
end

local shown_hp, shown_max
local function follow_hero(hero)
    local hp, max = hero:hp() or 0, hero:max_hp() or 0
    if hp == shown_hp and max == shown_max then return end
    shown_hp, shown_max = hp, max
    doc:text('#hero', ('%s, level %d: %d / %d health'):format(hero:name() or 'Hero', hero:level() or 0, hp, max))
    doc:style('#hp', 'width', ('%d%%'):format(max > 0 and math.floor(100 * hp / max) or 0))
end

gear.on('draw', function(state)
    if not doc then load() end
    if not doc or not visible then return end
    local hero = gear.world.hero()
    if hero then follow_hero(hero) end
    doc:draw()
end)

gear.keys.on('toggle', function()
    if not doc then return end
    visible = not visible
    if visible then doc:show() else doc:hide() end
end)
