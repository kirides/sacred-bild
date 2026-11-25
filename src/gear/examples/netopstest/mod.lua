return {
    id = 'netopstest',
    name = 'Custom net messages test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run: hosts a LAN game and tests gear.net. expect = "stock" (the original GameServer.exe): '
        .. 'no answer to HELLO, supported() stays false, gear.mp.send keeps working on the chat carrier. expect = "remake" '
        .. '(the SacredEngineRemake server with scripts-examples/net_echo.lua): handshake, limits, a table and a string through '
        .. 'the server\'s echo channel, 100 KB both ways, the chat carrier next to it.',
    automation = true,
    config = { hero_x = 820, hero_y = 400, expect = 'stock', chat_only = false },
}
