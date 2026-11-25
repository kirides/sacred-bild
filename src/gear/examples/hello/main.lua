-- An example mod: logs that it loaded, what the creature type table holds, and the frame rate every ten seconds.
-- Everything it prints goes to SacredGear.log.

gear.on('load', function()
    print('hello from ' .. gear.mod.name .. ' ' .. gear.mod.version .. ' on Sacred Gear ' .. gear.version)
end)

local reported = false
local frames, elapsed = 0, 0

gear.on('frame', function(dt)
    -- The creature types are there once a game runs.
    if not reported then
        local creatures = gear.data.creature_types
        local count = creatures:count()
        if count > 0 then
            reported = true
            print(('creature types: %d records of %d bytes'):format(count, creatures:stride()))
            local first = creatures:get(0)
            print(('record 0 starts with %d, %d'):format(first:i32(0), first:i32(4)))
        end
    end

    frames = frames + 1
    elapsed = elapsed + dt
    if elapsed >= 10 then
        print(('%.1f frames per second'):format(frames / elapsed))
        frames, elapsed = 0, 0
    end
end)
