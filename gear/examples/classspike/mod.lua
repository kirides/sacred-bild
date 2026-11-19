return {
    id = 'classspike',
    name = 'Class spike',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Automation run of the custom-class hooks: a Seraphim casts Battle Mage spells and uses Gladiator moves '
        .. 'with and without ability.route, a Gladiator casts a spell with a foreign and a remapped clip. Needs a single player '
        .. 'savegame with hostile creatures nearby (-gear-auto -gear-load=N -gear-mods=classspike).',
    automation = true,
    config = { quit = true },
}
