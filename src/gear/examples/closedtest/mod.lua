return {
    id = 'closedtest',
    name = 'Closed Net test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run that drives Multiplayer > Closed Internet against a test lobby (login, character, join the game server) for end-to-end tests.',
    automation = true,
    config = {
        host = '127.0.0.1', port = 17066,
        account = 'stasha', password = 'test1234',
        hero = 'Stasha',
        game_password = '1234', -- the game server's NETWORK_PASSWORD (Gameserver.cfg)
        create = false,         -- make a character first (a new account has none)
        expect_backpack = nil,  -- regression check: the number of backpack items the hero must have (exit code 1 otherwise)
        buy_button = nil,       -- 'rbutton': buy with a right click instead of picking up and dropping
        keep_class = false,     -- create: keep the class the screen starts with (Dark Elf) instead of picking the Gladiator
        stop = false,           -- 'games': stop at the game list
        quit = true,
    },
}
