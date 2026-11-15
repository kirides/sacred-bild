return {
    id = 'layout',
    name = 'Layout editor',
    version = '0.1.0',
    priority = 100,         -- loads last: its handles lie over the other mods' panels
    automation = true,      -- only for its self test (-gear-auto)
    config = {
        key = 'f7',         -- toggles the edit mode
    },
}
