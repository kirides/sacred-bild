---@type GearModManifest
return {
    id = 'lootlog',
    name = 'Loot log',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'A log of the items that dropped near the hero and the items that ended up in the backpack, filterable by quality, category and kind; kept between sessions.',
    settings = {
        {id = 'radius', type = 'number', label = 'Watch the ground within (tiles)', min = 5, max = 60, step = 5, default = 25, section = 'General',
            tooltip = 'Items that appear on the ground within this distance of the hero are logged as drops'},
        {id = 'max_entries', type = 'number', label = 'Entries kept', min = 50, max = 2000, step = 50, default = 500, section = 'General'},
        {id = 'log_normal', type = 'bool', label = 'Log plain (normal quality) items', default = false, section = 'General',
            tooltip = 'Off: only magic, rare, set, unique and quest items are logged'},
        {id = 'log_gold', type = 'bool', label = 'Log gold piles', default = false, section = 'General'},
    },
    keys = {
        {id = 'panel', label = 'Show / hide the loot log', default = 'f3'},
    },
}
