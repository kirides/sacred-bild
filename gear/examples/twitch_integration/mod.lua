return {
    id = 'twitch_integration',
    name = 'Twitch Integration (base)',
    version = '0.1.0',
    -- The connector program (the one of the Gothic II / Risen versions) hosts this pipe; the game is its client.
    pipes = {'__TwitchIntegration_Kirides_Conn'},
    -- Defaults; config.lua next to this file overrides them.
    config = {
        interval_s = 0.5,   -- at most one event per interval
        max_age_s = 300,    -- an event that waited longer than this outside a game is dropped (0: never)
        defer_s = 120,      -- how long an event an action mod deferred ("not now") is offered again
        max_deferred = 40,
    },
}
