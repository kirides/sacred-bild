-- What Twitch events do in the game: each entry is a list of actions "command arguments" (each action is one
-- twitch.command event for the action mods, see twi_sacred). Instead of the list an entry may be
-- { actions = {...}, cooldown_s = 120 } (chat: the same text is ignored for that long).
--
--   rewards  channel-point reward title -> actions (any case)
--   perks    StreamElements perk name   -> actions (any case)
--   bits     exact number of bits       -> actions
--   chat     exact chat text            -> actions (the connector forwards texts starting with "#")
return {
    rewards = {
        ['Spawn Monster'] = {'TWI_SpawnRandomMonster 1'},
        ['Monster Raid'] = {'TWI_Raid 3'},
        ['Teleport'] = {'TWI_RandomTeleport'},
    },
    perks = {
        ['Gold'] = {'TWI_Money 500'},
        ['Tax'] = {'TWI_Money -500'},
    },
    bits = {
        [50] = {'TWI_SpawnRandomMonster 1'},
        [100] = {'TWI_SpawnRandomMonster 2', 'TWI_Money 100'},
    },
    chat = {
        ['#hp_1'] = {actions = {'TWI_SetHP 1'}, cooldown_s = 120},
        ['#hp_1_5s'] = {actions = {'TWI_SetHP_Timed 1 5'}, cooldown_s = 120},
        ['#heal'] = {actions = {'TWI_FullHeal'}, cooldown_s = 300},
        ['#raid'] = {actions = {'TWI_Raid'}, cooldown_s = 120},
        ['#donate_100'] = {actions = {'TWI_Money -100'}, cooldown_s = 600},
    },
}
