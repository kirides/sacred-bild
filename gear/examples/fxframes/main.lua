-- Casts each ability of config.abilities (the hero learns it at config.level first), and takes a screenshot at every time of config.times after the
-- cast: GearAutomation\<instance>\fx-<ability>-<n>.bmp. The log has "FXF" lines: the time of each shot (real seconds after the order) and every
-- effect the engine made (type, time, creature, parent), to put frames and effect types side by side.
if not gear.auto then return end
local A = gear.auto
local cfg = gear.config

local t0 = 0
local function note(ctx)
    print(('FXF effect t=%.2f fx=0x%X creature=%s ability=%s level=%s parent=%s'):format(gear.time() - t0, ctx.fx,
        tostring(ctx.creature and ctx.creature.id), tostring(ctx.ability), tostring(ctx.level), tostring(ctx.parent)))
end

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then A.quit(1) return end
    gear.wait(3)
    local h = gear.world.hero()
    for _, id in ipairs(cfg.abilities) do
        if not h:ability(id) then h:learn(id, cfg.level or 5) end
    end
    gear.wait(6)
    gear.pfx.stats()
    local types = {}
    for t = 0x300, 0x49D do types[#types + 1] = t end
    -- a rule that never changes anything but makes the engine ask (so the notes see every effect)
    if cfg.tint then gear.pfx.define({id = 1, colorize = cfg.tint}) end
    local rule = gear.pfx.rule({fx = types, select = function(ctx) note(ctx) return cfg.tint and 1 or nil end})
    for _, id in ipairs(cfg.abilities) do
        local x, y = h:position()
        for _ = 1, 60 do
            local a = h:ability(id)
            if a and a.ready then break end
            gear.wait(0.5)
        end
        print(('FXF cast %d'):format(id))
        t0 = gear.time()
        local ok, err = h:order_ability(id, x + 250, y)
        print(('FXF order %s %s'):format(tostring(ok), tostring(err)))
        local last = 0
        for i, at in ipairs(cfg.times) do
            gear.wait(math.max(0, at - (gear.time() - t0)))
            local real = gear.time() - t0
            A.screenshot(('fx-%d-%d'):format(id, i))
            print(('FXF shot %d n=%d t=%.2f (wanted %.2f)'):format(id, i, real, at))
        end
        gear.wait(8)
    end
    gear.pfx.remove_rule(rule)
    A.quit(0)
end)
