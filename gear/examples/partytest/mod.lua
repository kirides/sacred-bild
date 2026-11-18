return {
    id = 'partytest',
    name = 'Party test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run of gear.party: recruits a companion, switches control back and forth, gives orders, '
        .. 'shares experience, saves and loads, dismisses. Needs a single player savegame with hostile creatures nearby.',
    automation = true,
    config = {
        save_slot = 63,     -- the slot the save and load check uses (overwritten)
        quit = true,
    },
}
