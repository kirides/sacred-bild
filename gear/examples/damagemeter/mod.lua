---@type GearModManifest
return {
    id = 'damagemeter',
    name = 'Damage meter',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Damage dealt and taken by the hero (from the engine\'s final damage of every hit), DPS over the fight and the last seconds (compact view by default: character name and DPS, the whole party in a network game), the biggest hit, damage by monster type, kills per minute.',
    settings = {
        {id = 'combat_timeout', type = 'number', label = 'A fight ends after (seconds without damage)', min = 2, max = 30, step = 1, default = 5, section = 'General'},
        {id = 'view', type = 'choice', label = 'View', default = 'compact', section = 'General',
            choices = {{value = 'compact', label = 'Compact (name and DPS)'}, {value = 'extended', label = 'Extended (DPS, taken, dealt)'}, {value = 'detailed', label = 'Detailed (everything)'}}},
        {id = 'share', type = 'bool', label = 'Share my DPS with the other players (network game)', default = true, section = 'General'},
        {id = 'window', type = 'number', label = 'Rolling DPS window (seconds)', min = 3, max = 30, step = 1, default = 10, section = 'General'},
    },
    keys = {
        {id = 'panel', label = 'Show / hide the damage meter', default = 'f7'},
        {id = 'view', label = 'Next view (compact, extended, detailed)', default = ''},
        {id = 'reset', label = 'Reset the damage meter', default = ''},
    },
}
