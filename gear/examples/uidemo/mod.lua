return {
    id = 'uidemo',
    name = 'UI drawing demo',
    version = '0.2.0',
    author = 'Sacred Gear',
    description = 'Shows what gear.ui can draw: a hero panel, a widget gallery, images, fonts.',
    automation = true,      -- only for its self test (-gear-auto); as a normal mod it just draws
    settings = {
        {id = 'panel', type = 'bool', label = 'Hero panel', default = true, tooltip = 'The hero panel at the top left'},
        {id = 'images', type = 'bool', label = 'Image fit modes', default = true, tooltip = 'The image fit modes beside the hero panel'},
        {id = 'inventory', type = 'bool', label = 'Inventory note', default = true, tooltip = 'A note under the inventory while it is open'},
        {id = 'fonts', type = 'bool', label = 'Font samples', default = false, tooltip = 'A sample line of every UI font (to pick one)'},
    },
    keys = {
        {id = 'gallery', label = 'Open the widget gallery', default = 'f9'},
    },
    config = {
        -- self test only
        settle = 1.5,
        quit = true,
    },
}
