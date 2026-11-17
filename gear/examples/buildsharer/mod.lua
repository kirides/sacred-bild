return {
    id = 'buildsharer',
    name = 'Build sharer',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Publish your hero build to the game server and browse the builds other players shared (F7). '
        .. 'Needs the Sacred.GameServer of the SacredEngineRemake with scripts-examples/build_sharer.lua; the server reads your skills and '
        .. 'equipment itself, this mod only sends a title, a note, tags and the items you carry.',
    settings = {
        {id = 'title', type = 'text', label = 'Build title', default = '', max_length = 40, section = 'My build',
            tooltip = 'A short name for your build, shown in the list'},
        {id = 'note', type = 'text', label = 'Note', default = '', max_length = 120, section = 'My build',
            tooltip = 'A line about the build (how it plays, what to level)'},
        {id = 'tags', type = 'text', label = 'Tags', default = '', max_length = 80, section = 'My build',
            tooltip = 'Comma separated, up to 5 words (pvp, bow, tank ...)'},
        {id = 'share_carried', type = 'bool', label = 'Share carried items', default = true, section = 'My build',
            tooltip = 'Send the names of the best items in your backpack (up to 12). The server lists them as "reported". Off: none'},
    },
    keys = {
        {id = 'toggle', label = 'Open the build sharer', default = 'f7'},
    },
    config = {
        page_size = 12,
    },
}
