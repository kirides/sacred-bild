return {
    id = 'mmtest-aux',
    name = 'Mod manager test: helper',
    version = '1.2',
    author = 'Sacred Gear',
    description = 'Draws a line of text while it runs, so switching it off in the Mods screen is visible.',
    settings = {
        {id = 'text', type = 'text', label = 'The line it draws', default = 'The helper mod is running', max_length = 40},
    },
    keys = {
        {id = 'toggle', label = 'Hide or show the line', default = 'ctrl+f3'},
    },
}
