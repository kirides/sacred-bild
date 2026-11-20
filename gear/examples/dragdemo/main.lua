-- gear.ui.drag_source / drop_target / world_drop: a list of creature names, three slots and the world.
local ui = gear.ui

local NAMES = {'Wolf', 'Skeleton', 'Orc', 'Troll'}
local slots = {nil, nil, nil}
local LOCKED = 3

gear.on('load', function()
    ui.world_drop('spawn', {
        accept = 'creature',
        hint = 'Spawn here',
        on_drop = function(drag, spot)
            local c, err = gear.world.spawn(drag.data, spot.x, spot.y)
            print(c and ('spawned ' .. drag.data) or ('spawn failed: ' .. tostring(err)))
        end,
    })
end)

gear.on('draw', function(state)
    if not state or state.mode ~= 'game' then return end
    local p = ui.panel('main', 'Drag and drop demo', 60, 200, 270, 150)
    if not p.open then return end
    for i, name in ipairs(NAMES) do
        local x, y, w, h = p.content_x + 8, p.content_y + 8 + (i - 1) * 24, 110, 20
        local r = ui.region('n' .. i, x, y, w, h)
        local dragging = ui.drag_source('n' .. i, r, {type = 'creature', data = name, label = name})
        ui.rect(x, y, w, h, dragging and 0x60303040 or (r.hover and 0xC0383850 or 0xC0202030))
        ui.text(name, x + 6, y + 3)
    end
    for i = 1, 3 do
        local x, y, w, h = p.content_x + 140, p.content_y + 8 + (i - 1) * 44, 100, 38
        local r = ui.region('slot' .. i, x, y, w, h)
        local t = ui.drop_target('slot' .. i, {
            accept = function(drag)
                if i == LOCKED then return false, 'This slot is locked' end
                if drag.type ~= 'creature' then return false, 'Only creatures' end
                return true, 'Put ' .. drag.data .. ' here'
            end,
            on_drop = function(drag) slots[i] = drag.data end,
        })
        local fill = 0xC0181820
        if t.dragging and t.valid then fill = t.over and 0xC0285a30 or 0xC0203a28 end
        if t.dragging and t.over and not t.valid then fill = 0xC05a2828 end
        ui.rect(x, y, w, h, fill)
        ui.frame(x, y, w, h, 0xFFC8AA5A)
        ui.text(i == LOCKED and 'Locked' or (slots[i] or 'Empty'), x + 8, y + 10)
        -- a click on a filled slot (not the one that just put something down) empties it
        if r.clicked and slots[i] and not ui.drag_active() then slots[i] = nil end
    end
end)
