return {
    id = 'lootfilter',
    name = 'Loot filter',
    version = '0.2.0',
    author = 'Sacred Gear',
    description = 'Picks up only the items you want: by class, quality and category; dims the rest on the ground.',
    automation = true,      -- only for its self test (-gear-auto)
    -- Defaults of what the Mods screen does not list; config.lua in this folder overrides them (see config.lua and
    -- docs/Gear.md). The rules themselves are the settings below, changed in the Mods screen or in the filter's own
    -- panel (F5 or the overlay's button); they used to be saved to settings.json, which is migrated once.
    config = {
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
        enabled = true,
        keep_quest = true,
        keep_set = false,
        dim_alpha = 70,
        overlay = true,
        overlay_x = 16,     -- the overlay's position in the 1024x768 layout
        overlay_y = 150,
        radius = 40,        -- tiles around the hero the filter looks at
        -- self test only
        test_quit = true,
    },
    settings = {
        {id = 'enabled', type = 'bool', label = 'Filter on', default = true, section = 'General', tooltip = 'Off: the hero picks up everything'},
        {id = 'overlay', type = 'bool', label = 'Show overlay', default = true, section = 'General',
            tooltip = 'The rules overlay, shown while Alt shows the labels. Off: never (the panel stays on its key)'},
        {id = 'radius', type = 'number', label = 'Look around the hero (tiles)', min = 5, max = 100, step = 5, default = 40, section = 'General'},
        {id = 'dim_alpha', type = 'number', label = 'Dim filtered labels', min = 0, max = 255, step = 5, default = 70, section = 'General',
            tooltip = 'Alpha of the labels of filtered items (255: not dimmed)'},
        {id = 'keep_quest', type = 'bool', label = 'Quest items always', default = true, section = 'General'},
        {id = 'keep_set', type = 'bool', label = 'Set items always', default = false, section = 'General'},
        {id = 'classes_mode', type = 'choice', label = 'Pick up items for', default = 'own', section = 'Classes',
            choices = {{value = 'own', label = 'My class'}, {value = 'any', label = 'Any class'}, {value = 'chosen', label = 'These classes'}}},
        {id = 'class_seraphim', type = 'bool', label = 'Seraphim', default = false, section = 'Classes'},
        {id = 'class_gladiator', type = 'bool', label = 'Gladiator', default = false, section = 'Classes'},
        {id = 'class_battlemage', type = 'bool', label = 'Battlemage', default = false, section = 'Classes'},
        {id = 'class_darkelf', type = 'bool', label = 'Darkelf', default = false, section = 'Classes'},
        {id = 'class_woodelf', type = 'bool', label = 'Woodelf', default = false, section = 'Classes'},
        {id = 'class_vampiress', type = 'bool', label = 'Vampiress', default = false, section = 'Classes'},
        {id = 'class_dwarf', type = 'bool', label = 'Dwarf', default = false, section = 'Classes'},
        {id = 'class_daemon', type = 'bool', label = 'Daemon', default = false, section = 'Classes'},
        {id = 'min_quality', type = 'choice', label = 'Lowest quality', default = 'normal', section = 'Quality',
            choices = {'normal', 'magic', 'rare', 'set', 'unique'}},
        {id = 'cat_weapon', type = 'choice', label = 'Weapon', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_armor', type = 'choice', label = 'Armor', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_shield', type = 'choice', label = 'Shield', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_jewelry', type = 'choice', label = 'Jewelry', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_rune', type = 'choice', label = 'Rune', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_scroll', type = 'choice', label = 'Scroll', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_special', type = 'choice', label = 'Special', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'cat_other', type = 'choice', label = 'Other', default = '(minimum)', section = 'Quality',
            choices = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}},
        {id = 'potion_health', type = 'bool', label = 'Health', default = true, section = 'Potions'},
        {id = 'potion_mana', type = 'bool', label = 'Mana', default = true, section = 'Potions'},
        {id = 'potion_stamina', type = 'bool', label = 'Stamina', default = true, section = 'Potions'},
        {id = 'potion_colored', type = 'bool', label = 'Colored', default = true, section = 'Potions', tooltip = 'The black, blue, green, red and yellow potions'},
        {id = 'potion_other', type = 'bool', label = 'Other', default = true, section = 'Potions'},
        {id = 'potion_color_black', type = 'choice', label = 'Black potions', default = 'default', section = 'Potions',
            choices = {{value = 'default', label = 'As their kind'}, {value = 'take', label = 'Pick up'}, {value = 'leave', label = 'Leave'}}},
        {id = 'potion_color_blue', type = 'choice', label = 'Blue potions', default = 'default', section = 'Potions',
            choices = {{value = 'default', label = 'As their kind'}, {value = 'take', label = 'Pick up'}, {value = 'leave', label = 'Leave'}}},
        {id = 'potion_color_green', type = 'choice', label = 'Green potions', default = 'default', section = 'Potions',
            choices = {{value = 'default', label = 'As their kind'}, {value = 'take', label = 'Pick up'}, {value = 'leave', label = 'Leave'}}},
        {id = 'potion_color_red', type = 'choice', label = 'Red potions', default = 'default', section = 'Potions',
            choices = {{value = 'default', label = 'As their kind'}, {value = 'take', label = 'Pick up'}, {value = 'leave', label = 'Leave'}}},
        {id = 'potion_color_yellow', type = 'choice', label = 'Yellow potions', default = 'default', section = 'Potions',
            choices = {{value = 'default', label = 'As their kind'}, {value = 'take', label = 'Pick up'}, {value = 'leave', label = 'Leave'}}},
    },
    keys = {
        {id = 'panel', label = 'Open the filter panel', default = 'f5'},
    },
}
