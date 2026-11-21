return {
    id = 'modmanagertest',
    name = 'Mod manager test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run that drives the Mods screen: pages, switches, settings, key binds.',
    automation = true,
    -- Every kind of setting, so the screen shows them all.
    settings = {
        {id = 'flag', type = 'bool', label = 'A checkbox', default = false, tooltip = 'type bool'},
        {id = 'level', type = 'number', label = 'A slider', min = 0, max = 10, step = 1, default = 3, tooltip = 'type number'},
        {id = 'mode', type = 'choice', label = 'A dropdown', choices = {'first', 'second', {value = 'third', label = 'Third one'}},
         default = 'first', tooltip = 'type choice'},
        {id = 'name', type = 'text', label = 'A text field', default = 'abc', max_length = 16, tooltip = 'type text'},
        {id = 'tint', type = 'color', label = 'A color', default = 0xFF3080C0, tooltip = 'type color'},
    },
    keys = {
        {id = 'ping', label = 'Ping', default = 'ctrl+f2'},
        {id = 'pong', label = 'Pong', default = ''},
    },
    config = {
        quit = true,
        pad_list = false,       -- the D-pad in the list: needs more mods than the list has room for (-gear-mods=... with a few more)
    },
}
