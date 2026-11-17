---@type GearModManifest
return {
    id = 'sessiontracker',
    name = 'Session tracker',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Time played, kills, gold and experience gained this session, with per-hour rates; lifetime totals and the last sessions are kept.',
    settings = {
        {id = 'min_rate_seconds', type = 'number', label = 'Show per-hour rates after (seconds)', min = 5, max = 600, step = 5, default = 30,
            section = 'General', tooltip = 'Rates of a very short session swing wildly; they show as "-" until this much time has been played'},
        {id = 'save_interval', type = 'number', label = 'Save totals every (seconds)', min = 15, max = 600, step = 15, default = 60, section = 'General'},
    },
    keys = {
        {id = 'panel', label = 'Show / hide the session panel', default = 'f2'},
    },
}
