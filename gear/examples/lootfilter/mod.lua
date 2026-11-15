return {
    id = 'lootfilter',
    name = 'Loot filter',
    version = '0.1.0',
    automation = true,      -- only for its self test (-gear-auto)
    -- The defaults; config.lua in this folder overrides them (see config.lua and docs/Gear.md).
    config = {
        enabled = true,
        -- Pick up only items the hero can use: 'own' (the hero's class), or a list of classes:
        -- {'seraphim', 'gladiator', 'battlemage', 'darkelf', 'woodelf', 'vampiress', 'dwarf', 'daemon'}.
        -- false: any class.
        classes = 'own',
        -- The lowest quality picked up: 'normal', 'magic', 'rare', 'set', 'unique' (ascending).
        min_quality = 'normal',
        -- Per category, overriding min_quality; false: never pick up. Categories: weapon, armor, shield, jewelry,
        -- rune, scroll, special, other.
        categories = {},
        -- Potions by kind (false: leave them): health, mana, stamina, colored (the black, blue, green, red and yellow
        -- ones; a colour as key decides for that colour: red = false), other.
        potions = {health = true, mana = true, stamina = true, colored = true, other = true},
        keep_quest = true,  -- quest items always
        keep_set = false,   -- set items always, whatever their category's rule
        -- Ground labels of filtered items: dimmed to this alpha (0..255; 255: unchanged).
        dim_alpha = 70,
        -- The overlay with the current rules: false to hide it; position in the 1024x768 layout.
        overlay = true,
        overlay_x = 16,
        overlay_y = 150,
        radius = 40,        -- tiles around the hero the filter looks at
        -- self test only
        test_quit = true,
    },
}
