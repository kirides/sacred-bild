-- A mod for the mod manager's self test (src/gear/examples/modmanagertest): it only draws a line of text.
local hidden = false
gear.keys.on('toggle', function() hidden = not hidden end)
gear.on('settings.changed', function(id, value) print('setting ' .. id .. ' = ' .. tostring(value)) end)
gear.on('unload', function() print('unload') end)
gear.on('draw', function(state)
    if state.mode ~= 'game' or hidden then return end
    gear.ui.text(gear.settings.get('text'), 16, 700, {color = 0xFF90E890, shadow = true, background = 0xA0000000, padding = 3})
end)
