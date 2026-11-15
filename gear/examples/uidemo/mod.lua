return {
    id = 'uidemo',
    name = 'UI drawing demo',
    version = '0.1.0',
    automation = true,      -- only for its self test (-gear-auto); as a normal mod it just draws
    config = {
        panel = true,       -- the hero panel at the top left
        images = true,      -- the image fit modes beside it
        inventory = true,   -- a note under the inventory while it is open
        fonts = false,      -- a sample line of every UI font (to pick one)
        -- self test only
        settle = 1.5,
        quit = true,
    },
}
