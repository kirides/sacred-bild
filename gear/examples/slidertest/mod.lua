return {
    id = 'slidertest',
    name = 'Slider test',
    version = '0.1.0',
    author = 'Sacred Gear',
    description = 'An automation run: slider settings of different ranges keep their values when the Mods screen reuses rows.',
    automation = true,
    settings = {
        {id = 'a', type = 'number', label = 'Companions at most', min = 1, max = 14, step = 1, default = 3},
        {id = 'b', type = 'number', label = 'Shared experience', min = 0, max = 100, step = 5, default = 100},
        {id = 'c', type = 'number', label = 'Heal below', min = 10, max = 95, step = 5, default = 60},
        {id = 'd', type = 'number', label = 'Spread', min = 0, max = 6, step = 1, default = 1},
        {id = 'e', type = 'number', label = 'Follow distance', min = 2, max = 12, step = 1, default = 5},
    },
}
