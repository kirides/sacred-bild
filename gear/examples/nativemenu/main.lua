-- The game menu (Esc in game) as an RmlUi document instead of the game's own window.
--
--   * gear.ui.replace_window hides the game's window while this screen is up; the window still opens and closes as usual
--     (Esc, the game's own commands), so the game's state stays what it expects.
--   * The entries are the game's own: gear.ui.menu_entries reads their text (in the game's language), gear.ui.menu_click presses
--     one as the game's own click would (options, save, quit with its question ...).
--   * After them come the items other mods registered with gear.menu.add_item('esc', ...) (the Mods manager's "Mods" is one).
--     Mods that want to work with and without this screen draw a button of their own only while gear.menu.available('esc')
--     is false.
--   * Every entry is a pad stop and the pad's B closes the menu; the layout is flexbox in dp units, so it scales with the UI.
--
-- The look is ui/menu.rcss: replace it to restyle the screen.

local ui, menu = gear.ui, gear.menu

local WINDOW = 'UI_WND_ESCMENU'

local doc, signature
local entries, items = {}, {}

local function escape(text)
    return (tostring(text):gsub('&', '&amp;'):gsub('<', '&lt;'):gsub('>', '&gt;'))
end

-- What the screen shows changes only when the game menu or the registered items do: rebuild the document then.
local function rebuild()
    local parts = {}
    for _, e in ipairs(entries) do parts[#parts + 1] = 'v:' .. e.text end
    for _, it in ipairs(items) do parts[#parts + 1] = 'm:' .. it.id .. ':' .. it.label .. (it.enabled and '' or ':off') end
    local sig = table.concat(parts, '\0')
    if doc and sig == signature then return end
    signature = sig
    if doc then doc:close(); doc = nil end

    local html = {'<rml><head><link type="text/rcss" href="ui/menu.rcss"/></head><body><div class="backdrop"><div class="panel">'}
    for i, e in ipairs(entries) do
        html[#html + 1] = ('<button id="v%d" class="entry">%s</button>'):format(i, escape(e.text))
    end
    if #items > 0 then html[#html + 1] = '<div class="rule"></div>' end
    for i, it in ipairs(items) do
        html[#html + 1] = ('<button id="m%d" class="entry mod"%s>%s</button>'):format(i, it.enabled and '' or ' disabled', escape(it.label))
    end
    html[#html + 1] = '</div></div></body></rml>'

    local d, err = ui.rml.load_string(table.concat(html), {dp = true})
    if not d then
        print('nativemenu: ' .. tostring(err))
        return
    end
    d:on('.entry', 'click', function(e)
        local kind, n = e.current:match('^(%a)(%d+)$')
        n = tonumber(n)
        if kind == 'v' then
            ui.menu_click(WINDOW, n)
        elseif kind == 'm' and items[n] then
            items[n].click()
        end
    end)
    doc = d
end

gear.on('load', function()
    menu.host('esc')
    ui.replace_window(WINDOW, true)
end)

gear.on('unload', function()
    ui.nav_modal(false)
    ui.replace_window(WINDOW, false)
    menu.host('esc', false)
    if doc then doc:close(); doc = nil end
end)

gear.on('draw', function()
    if not ui.window_open(WINDOW) then
        ui.nav_modal(false)
        return
    end
    entries = ui.menu_entries(WINDOW)
    if #entries == 0 then return end
    items = menu.items('esc')
    rebuild()
    -- Only a document that is there takes the game's window's place: without one the game's own menu shows.
    if not doc or not ui.replaced(WINDOW) then
        ui.nav_modal(false)
        return
    end
    ui.nav_modal(true)
    if ui.back_pressed() then
        ui.close_window(WINDOW)
        return
    end
    doc:draw({layer = 'top'})
end)
