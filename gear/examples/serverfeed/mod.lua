return {
    id = 'serverfeed',
    name = 'Server feed',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Shows the announcements of the SacredEngineRemake game server on screen: rare and unique drops '
        .. '(scripts-examples/rare_drops.lua) and hero deaths (graveyard.lua), and a list of the latest of both. '
        .. 'Does nothing on a server without those scripts.',
    settings = {
        {id = 'drops', type = 'bool', label = 'Rare drops', default = true, tooltip = 'A line when somebody finds a rare or unique item'},
        {id = 'deaths', type = 'bool', label = 'Hero deaths', default = true, tooltip = 'A line when a hero dies'},
        {id = 'seconds', type = 'number', label = 'Seconds on screen', min = 3, max = 30, step = 1, default = 8},
        {id = 'x', type = 'number', label = 'Position: right edge', min = 200, max = 1024, step = 8, default = 1008},
        {id = 'y', type = 'number', label = 'Position: top', min = 0, max = 700, step = 8, default = 180},
    },
    keys = {
        {id = 'history', label = 'Show the latest drops and deaths', default = 'f7'},
    },
    config = {},
}
