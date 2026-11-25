return {
    id = 'itemicontest',
    name = 'Item icon test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Draws a grid of item models with gear.ui.item_icon: by type id and by item.',
    automation = true,
    config = {
        quit = true,
        mode = 'both',  -- 'live' (items only), 'type' (type ids only) or 'both'
        icons = 6,      -- how many weapon / armour types per row
    },
}
