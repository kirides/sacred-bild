---@type GearModManifest
return {
    id = 'achievements',
    name = 'Achievements and login streak',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Toasts for the achievements and the daily login streak of the game server\'s achievements.lua '
        .. '(Sacred.GameServer scripts-examples), and a panel listing them with progress. Needs that server; '
        .. 'the original GameServer.exe does not talk to mods.',
    settings = {
        {id = 'toasts', type = 'bool', label = 'Show toasts', default = true, tooltip = 'A message at the top of the screen for every unlock and the daily login reward'},
        {id = 'seconds', type = 'number', label = 'Toast time (seconds)', default = 6, min = 2, max = 15, step = 1, tooltip = 'How long a toast stays'},
    },
    keys = {
        {id = 'panel', label = 'Open the achievements panel', default = 'f7'},
    },
    config = {
        -- the panel's default place in the 1024x768 layout (the player can drag it)
        panel_x = 590, panel_y = 40,
    },
}
