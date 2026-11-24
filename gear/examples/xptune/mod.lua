return {
    id = 'xptune',
    name = 'Creature and experience tuning',
    version = '1.0.0',
    author = 'Sacred Gear',
    description = 'Example: scales the experience the hero gains and makes creatures tougher or weaker, all (or only chosen types).',
    -- Everything here is read as gear.config. Edit the numbers, not the code.
    config = {
        xp_scale = 2.0,         -- experience from kills and quests: 2 = double, 0.5 = half, 0 = none
        health = 1.5,           -- maximum health of the engine's creatures, as a multiple (1 = unchanged)
        damage = 1.0,           -- damage they deal, as a multiple
        taken = 1.0,            -- damage they take, as a multiple (0.5 = they shrug off half)
        -- Per type overrides by internal name (see the log, or gear.data.object_types:search): {health=..., damage=..., taken=...}
        types = {
            -- wolf = {health = 3, damage = 2},
        },
        min_level = 0,          -- only creatures of at least this level are changed
    },
}
