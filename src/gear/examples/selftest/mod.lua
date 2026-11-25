return {
    id = 'selftest',
    name = 'Sacred Gear self test',
    version = '0.1.0',
    -- gear.auto: only when the game runs with -gear-auto.
    automation = true,
    config = {
        timeout_menu = 120,     -- seconds to wait for the main menu
        timeout_game = 180,     -- seconds to wait for the game after loading
        spawn_type = '',        -- an object type name to spawn once (empty: the spawn test is skipped)
        quit = true,            -- end the game when done (exit code 0: all checks passed, 1: some failed)
    },
}
