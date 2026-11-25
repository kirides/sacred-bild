-- Hooks between mods. Other mods publish events with gear.emit; this one listens, and publishes its own.
-- Nothing here depends on those mods: a handler for an event nobody emits never runs, so the load order does not matter.

-- 1. One hook, by name. The payload is one table (see the publishing mod's README for its fields).
gear.on('bounty.completed', function(ev)
    gear.toast.show('Well done!', ev.title .. ' (' .. ev.target .. ')')
end)

-- 2. Every hook of a mod: `prefix.*` handlers get the event name first.
gear.on('worldevent.*', function(event, ev)
    print(('%s: %s'):format(event, tostring(ev.name or ev.id)))
end)

-- 3. Publishing: declare the hook once (so other mods can find it with gear.events.list), emit it when it happens.
gear.events.declare('hooks-example.tick', {doc = 'Every 30 seconds.', args = 'n: how many so far'})
local n = 0
gear.every(30, function()
    n = n + 1
    gear.emit('hooks-example.tick', {n = n})
end)
gear.on('hooks-example.tick', function(ev) print('tick ' .. ev.n) end)

-- 4. What is published right now (name, mod, doc, args, number of listeners):
gear.on('load', function()
    for _, h in ipairs(gear.events.list()) do print(('hook %s by %s (%d listeners)'):format(h.name, h.mod, h.listeners)) end
end)
