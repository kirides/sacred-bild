return {
    id = 'bounty',
    name = 'Bounty board',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'Shows the bounties of a Sacred.GameServer with scripts-examples/bounty_board.lua: what to hunt, '
        .. 'your progress, the time left and the reward, in a panel (F8) and as a small tracker.',
    settings = {
        {id = 'tracker', type = 'bool', label = 'Tracker', default = true,
            tooltip = 'A few lines with the progress of your open bounties, at the top right'},
    },
    keys = {
        {id = 'panel', label = 'Open the bounty board', default = 'f8'},
    },
    config = {
        tracker_x = 780,    -- the tracker's place in the 1024x768 layout
        tracker_y = 150,
        refresh = 60,       -- seconds between asking the server for the board (it also pushes every change)
    },
}
