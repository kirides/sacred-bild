-- A mod for the mod manager's self test: it requires mmtest-aux and draws a line of text.
gear.on('draw', function(state)
    if state.mode ~= 'game' then return end
    gear.ui.text('The dependent mod is running', 16, 722, {color = 0xFFFFC060, shadow = true, background = 0xA0000000, padding = 3})
end)
