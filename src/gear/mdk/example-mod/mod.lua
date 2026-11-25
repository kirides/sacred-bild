---@type GearModManifest
-- The manifest: a table, evaluated without any globals. See GearModManifest in meta/gear.lua.
return {
    id = 'example-mod',                 -- the folder name should match; letters, digits, _ and -
    name = 'Example mod',
    version = '0.1.0',
    author = '',
    description = 'What this mod does, in a sentence.',
    -- priority = 0,             -- lower loads first
    -- requires = {},            -- ids of mods that must load before this one
    -- enabled = false,          -- keep the folder but skip the mod

    -- The defaults; the player's choices (Mods screen, F11) override them.
    config = {
        enabled = true,
        message = 'Hello from Example mod',
    },
    -- What the Mods screen shows. Read them with gear.settings.get(id); hear changes as 'settings.changed'.
    settings = {
        {id = 'enabled', type = 'bool', label = 'Show the message', default = true},
        {id = 'message', type = 'text', label = 'Message', default = 'Hello from Example mod', max_length = 60},
    },
    -- Key binds the player can change in the Mods screen.
    keys = {
        {id = 'toggle', label = 'Show or hide the message', default = 'ctrl+m'},
    },
}
