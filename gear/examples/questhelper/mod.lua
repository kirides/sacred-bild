return {
    id = 'questhelper',
    name = 'Quest helper',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'A panel (F8) with the quest items in your backpack (read here) and the quest records of your hero, with hints, '
        .. 'which the game server reports (Sacred.GameServer with scripts-examples/quest_helper.lua and hero data). '
        .. 'Limits: neither Gear nor the server can read the game\'s quest journal.',
    settings = {
        {id = 'notices', type = 'bool', label = 'Show change notices', default = true,
            tooltip = 'A short line on the screen when the server reports a new or changed quest record'},
        {id = 'refresh', type = 'number', label = 'Refresh while open (seconds)', min = 5, max = 120, step = 5, default = 30},
    },
    keys = {
        {id = 'toggle', label = 'Open the quest helper', default = 'f8'},
    },
    config = {},
}
