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
        create = false,         -- make a character first (a new account has none)
        quit = true,
    },
}
