return {
    id = 'saveinfo',
    name = 'Save info',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run that loads a savegame (config.slot) and writes where the hero stands and what is around (creatures by kind, items), to GearAutomation/<instance>/saveinfo.json.',
    automation = true,
    config = {slot = 1, radius = 3000},
}
