return {
    id = 'netopsspike',
    name = 'Custom opcode spike',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Stage 1 spike: hosts a LAN game (the stock GameServer.exe) and sends messages of the custom opcodes 0x1E0 / 0x1E1 to it; '
        .. 'checks that the connection and the game survive and that the chat carrier still works.',
    automation = true,
    config = { hero_x = 820, hero_y = 400, expect = 'stock' },
}

