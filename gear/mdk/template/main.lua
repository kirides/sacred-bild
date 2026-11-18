-- @NAME@: a starting point. Everything print() writes goes to SacredGear.log.
-- The `gear` global, its events and every function are declared in the MDK's meta folder (see README.md), so the
-- editor completes and checks what you type here.

local ui = gear.ui

local shown = true

gear.on('load', function()
    print(gear.mod.name .. ' ' .. gear.mod.version .. ' loaded on Sacred Gear ' .. gear.version)
end)

-- The key declared in mod.lua; the player may have rebound it.
gear.keys.on('toggle', function()
    shown = not shown
end)

-- 'draw' runs every frame after the 'frame' handlers; draw the UI here.
gear.on('draw', function()
    if not shown or not gear.settings.get('enabled') then return end
    ui.text(gear.settings.get('message'), 16, 16, {color = ui.rgba(255, 215, 120)})
    ui.text(('%s shows or hides this'):format(gear.keys.label('toggle')), 16, 34,
        {color = ui.rgba(255, 255, 255, 160)})
end)

gear.on('unload', function()
    -- The player switched the mod off: handlers, timers and key binds are dropped for you.
end)
