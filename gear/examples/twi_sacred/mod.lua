return {
    id = 'twi_sacred',
    name = 'Twitch Integration: actions',
    version = '0.1.0',
    requires = {'twitch_integration'},
    config = {
        -- Object type names (data.object_types) the monster actions pick from; names that are not found are
        -- logged and left out. Run the selftest example once: it writes every type name to GearAutomation\types.txt.
        monsters = {'Goblin', 'Wolf', 'Skeleton', 'Orc', 'Zombie'},
        -- How far from the hero spawned monsters appear, in world units (a tile is about 54; the screen shows
        -- about 534 x 400 around the hero): just out of sight.
        spawn_min = 380,
        spawn_max = 520,
    },
}
