return {
    id = 'layout',
    name = 'Layout editor',
    version = '0.2.0',
    author = 'Sacred Gear',
    description = "Move the game's windows and mod panels where you want them: an edit mode like an MMO's.",
    priority = 100,         -- loads last: its handles lie over the other mods' panels
    automation = true,      -- only for its self test (-gear-auto)
    settings = {
        {id = 'snap', type = 'bool', label = 'Snap to edges and frames', default = true,
         tooltip = 'Hold Shift while dragging to move freely'},
    },
}
