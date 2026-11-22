return {
    id = 'fxframes',
    name = 'Skill effect frame capture',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Automation run that casts spells/arts and takes screenshots at fixed times after the cast, and logs which effect types the engine made (-gear-auto -gear-load=N -gear-mods=fxframes). Config: abilities = list of ids, times = seconds after the cast.',
    automation = true,
    config = { quit = true, abilities = {5, 6, 12, 30}, times = {0.3, 0.6, 1.0, 1.4, 2.0, 2.4, 3.2, 4.5}, level = 5 },
}
