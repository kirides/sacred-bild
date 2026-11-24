-- Example mod: change creature values and the experience the hero gains.
--
-- What the API offers (docs/GearApi.md), and why this mod does it the way it does:
--
--  * READING values: `gear.data.creature_types` (Pak\Creature.pak) and `gear.data.object_types` are READ ONLY tables.
--    Nothing writes into them, so a mod does not edit "the creature's record".
--  * CHANGING a creature: the creature object is the thing you can change. `creature:set_modifiers{...}` sets its maximum
--    health and (through Gear's damage hook) the damage it deals and takes. The event `creature.created` hands you every
--    creature the engine itself makes (sector fills, spawn tables, scripts, respawns) once it is complete.
--    Creatures a savegame loads, and those `gear.world.spawn` makes, do not fire it: give spawn its own `modifiers` option.
--    (Modifiers are forgotten on a savegame load; creatures loaded from a save keep their boosted health only.)
--  * EXPERIENCE: `gear.party.experience_scale(factor)` multiplies every gain the game makes for a hero (kills, quests).
--    `creature:add_experience(n)` gives a bonus through the game's own gain; that one is not scaled again.
--    `creature:experience()` / `creature:level()` read the totals.
--  * balance.bin: the game-wide tables (the loot chances for example) live in `bin\balance.bin`. A mod changes them with a
--    file overlay (`overlay.txt` with `bin\balance.bin = patch:balance.lua`, see docs/Gear.md "Replacing game files").
--    That needs the exact word offsets of the table, which this example does not use: experience is scaled at the point
--    where the game awards it instead, which also works for quests and needs no file knowledge.
--
-- Single player only: in a network game the server decides health and experience.

local cfg = gear.config

-- The settings for one creature type: the global ones, with the entry of its internal name on top.
local function settings_for(type_name)
    local t = {health = cfg.health, damage = cfg.damage, taken = cfg.taken}
    local override = type_name and cfg.types[type_name:lower()]
    if override then
        for k, v in pairs(override) do t[k] = v end
    end
    return t
end

-- 1. Experience: one call on load; it stays in force while the game runs.
gear.on('load', function()
    gear.party.experience_scale(cfg.xp_scale)
    print(('xptune: experience x%s, creature health x%s'):format(cfg.xp_scale, cfg.health))
end)

-- 2. Creatures the engine makes. `e.creature` is the new creature, `e.type` its object type.
local changed = 0
gear.on('creature.created', function(e)
    local c = e.creature
    if (c:level() or 0) < cfg.min_level then return end
    local s = settings_for(gear.data.object_types:name(e.type))
    if s.health == 1 and s.damage == 1 and s.taken == 1 then return end
    -- `health` is a multiple of the creature's own maximum; use `max_hp = 500` for an absolute value instead.
    local ok, err = c:set_modifiers{health = s.health, damage = s.damage, taken = s.taken}
    if ok then
        changed = changed + 1
        if changed <= 5 then   -- a few lines in the log, not one per creature
            print(('xptune: %s (level %s) now has %s hp'):format(c:display_name() or '?', tostring(c:level()), tostring(c:max_hp())))
        end
    else
        print('xptune: set_modifiers refused: ' .. tostring(err))
    end
end)

-- 3. Reading the (read only) creature type table, to show what a mod can look at: the first record's words.
local reported = false
gear.on('frame', function()
    local types = gear.data.creature_types
    if not reported and types:count() > 0 then
        reported = true
        local r = types:get(0)
        print(('xptune: %d creature types of %d bytes; record 0 starts %d, %d'):format(types:count(), types:stride(), r:i32(0), r:i32(4)))
    end
end)
