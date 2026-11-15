return {
    id = 'uitest',
    name = 'UI self test',
    version = '0.1.0',
    automation = true,
    config = {
        timeout_menu = 120,
        timeout_game = 180,
        settle = 1.5,       -- seconds after a key before the screenshot
        quit = true,
    },
}
