return {
    id = 'mptest',
    name = 'Mod channel test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run: the hero\'s name and the host, and a 5000-byte message through the mod channel '
        .. '(single player: the loopback; mode lan: hosts a LAN game and sends to its own slot through the real chat carrier).',
    automation = true,
    config = {
        mode = 'sp',        -- 'sp' (load a save, loopback) or 'lan' (host a LAN game, the network path)
        hero_x = 820,       -- lan: the hero to pick in the LAN hero list (1600x900 screenshot pixels)
        hero_y = 400,
    },
}
