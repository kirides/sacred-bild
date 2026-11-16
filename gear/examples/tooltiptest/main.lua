-- Tooltip placement (gear.ui.tooltip_rect): a button with a tooltip in each corner and at the middle of each edge of
-- the screen; the run rests the cursor on each and takes a screenshot (look at them: the tooltip must never lie under
-- the cursor's arrow and never leave the screen).
--
-- Run isolated:  Sacred.exe -gear-auto -gear-load=1 -gear-instance=tips -gear-mods=tooltiptest -gear-timeout=120
-- Screenshots: GearAutomation\tips\tip-<place>.bmp

local A = gear.auto
local ui = gear.ui

local BW, BH = 70, 24
local PLACES = {'top-left', 'top', 'top-right', 'left', 'right', 'bottom-left', 'bottom', 'bottom-right'}

-- The button of a place, kept inside the screen.
local function spot(place)
    local l, t, r, b = ui.screen()
    local x = place:find('left') and l + 2 or place:find('right') and r - BW - 2 or (l + r - BW) / 2
    local y = place:find('top') and t + 2 or place:find('bottom') and b - BH - 2 or (t + b - BH) / 2
    if place == 'left' or place == 'right' then y = (t + b - BH) / 2 end
    return x, y
end

gear.on('draw', function()
    for _, place in ipairs(PLACES) do
        local x, y = spot(place)
        ui.button('tip:' .. place, x, y, BW, BH, (place:gsub('%-', ' ')):sub(1, 9),
            {layer = 'top', tooltip = 'Tooltip of the ' .. place .. ' button, long enough to need room'})
    end
end)

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(3)
    for _, place in ipairs(PLACES) do
        local x, y = spot(place)
        local px, py = A.ui_to_pixels(x + BW / 2, y + BH / 2)
        A.move(px, py)
        gear.wait(1.2)
        A.screenshot('tip-' .. place)
    end
    A.quit(0)
end)
