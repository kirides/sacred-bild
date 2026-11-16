return {
    id = 'servertest',
    name = 'Server overlay test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run: starts GameServer.exe as the game does for a hosted LAN game and quits, to check that the gameserver sees the overlaid files of the mods (see tools/server-overlay-test.ps1).',
    automation = true,
    config = {
        seconds = 8,
    },
}
