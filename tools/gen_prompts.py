# Builds src/overlay/prompts.png, the controller button prompts' texture atlas, from the Meritite Union's Input
# Prompts 2.0 (CC0, https://github.com/meritite-union/input-prompts), dark theme, solid fill.
# Layout (src/overlay/prompts.h relies on it): 64x64 cells, 8 per row; per pad style (Xbox, PlayStation, Switch) 18
# cells in the order of Gamepad::Button's bits (A B X Y LB RB LT RT Back Start L3 R3 Up Down Left Right), then the
# left and right stick; after the three styles one "+" for two-button bindings. Each icon is 60x60 in its cell (a
# transparent border keeps bilinear filtering from reaching the next one); colors are premultiplied by alpha.
#
# Source images: https://github.com/meritite-union/input-prompts (by @hergergy, CC0 1.0: free to use, no attribution
# required). Download release 2.0.0 (https://github.com/meritite-union/input-prompts/releases), extract it, and pass
# the extracted folder (the one containing png_256/). The tool reads only png_256/<xbox|ps|ns>/dark/.
# Needs Pillow (pip install pillow).
# Usage: python tools/gen_prompts.py [path to input-prompts-2.0.0]   (or set INPUT_PROMPTS)
import os, sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('INPUT_PROMPTS', r'B:\A\Downloads\input-prompts-2.0.0')
OUT = os.path.join(ROOT, 'src', 'overlay', 'prompts.png')

CELL, ICON, COLUMNS = 64, 60, 8

# Buttons by position, as Gamepad names them (south = A).
STYLES = {
    'xbox': [
        'button/primary/color/solid/a', 'button/primary/color/solid/b',
        'button/primary/color/solid/x', 'button/primary/color/solid/y',
        'button/bumper/_/solid/lb', 'button/bumper/_/solid/rb',
        'button/trigger/_/solid/lt', 'button/trigger/_/solid/rt',
        'button/secondary/_/solid/view', 'button/secondary/_/solid/menu',
        'button/stickclick/_/solid/ls', 'button/stickclick/_/solid/rs',
        'dpad/_/arrow/opaque/w', 'dpad/_/arrow/opaque/s', 'dpad/_/arrow/opaque/a', 'dpad/_/arrow/opaque/d',
        'stick/_/l/solid/_', 'stick/_/r/solid/_',
    ],
    'ps': [
        'button/primary/color/solid/cross', 'button/primary/color/solid/circle',
        'button/primary/color/solid/square', 'button/primary/color/solid/triangle',
        'button/bumper/_/solid/lb', 'button/bumper/_/solid/rb',
        'button/trigger/_/solid/l2', 'button/trigger/_/solid/r2',
        'button/secondary/_/solid/create', 'button/secondary/_/solid/options',
        'button/stickclick/_/solid/l3', 'button/stickclick/_/solid/r3',
        'dpad/_/arrow/opaque/w', 'dpad/_/arrow/opaque/s', 'dpad/_/arrow/opaque/a', 'dpad/_/arrow/opaque/d',
        'stick/_/l/solid/_', 'stick/_/r/solid/_',
    ],
    'ns': [
        'button/primary/_/solid/b', 'button/primary/_/solid/a',
        'button/primary/_/solid/y', 'button/primary/_/solid/x',
        'button/bumper/_/solid/l', 'button/bumper/_/solid/r',
        'button/trigger/_/solid/zl', 'button/trigger/_/solid/zr',
        'button/trigger/_/solid/minus', 'button/trigger/_/solid/plus',
        'button/stickclick/_/solid/l', 'button/stickclick/_/solid/r',
        'dpad/_/arrow/opaque/w', 'dpad/_/arrow/opaque/s', 'dpad/_/arrow/opaque/a', 'dpad/_/arrow/opaque/d',
        'stick/_/l/solid/_', 'stick/_/r/solid/_',
    ],
}


def icon(style, name):
    path = os.path.join(PACK, 'png_256', style, 'dark', *name.split('/')) + '.png'
    if not os.path.exists(path):
        sys.exit(f'missing: {path}')
    return Image.open(path).convert('RGBA').resize((ICON, ICON), Image.LANCZOS)


def plus():
    # Drawn 4x and scaled down: a light "+" with a dark rim, about half a button's size.
    big = Image.new('RGBA', (ICON * 4, ICON * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    c = ICON * 2
    for arm, half, color in ((ICON - 4, ICON // 4 + 12, (20, 16, 12, 200)), (ICON - 16, ICON // 4, (250, 247, 240, 255))):
        d.rounded_rectangle((c - half, c - arm, c + half, c + arm), radius=half, fill=color)
        d.rounded_rectangle((c - arm, c - half, c + arm, c + half), radius=half, fill=color)
    return big.resize((ICON, ICON), Image.LANCZOS)


def premultiply(im):
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            px[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)


def main():
    icons = [icon(style, name) for style, names in STYLES.items() for name in names] + [plus()]
    rows = (len(icons) + COLUMNS - 1) // COLUMNS
    size = 1
    while size < max(COLUMNS, rows) * CELL:
        size *= 2
    atlas = Image.new('RGBA', (COLUMNS * CELL, size), (0, 0, 0, 0))
    pad = (CELL - ICON) // 2
    for i, im in enumerate(icons):
        atlas.alpha_composite(im, ((i % COLUMNS) * CELL + pad, (i // COLUMNS) * CELL + pad))
    premultiply(atlas)
    atlas.save(OUT, optimize=True)
    print(f'{OUT}: {len(icons)} prompts, {atlas.width}x{atlas.height}, {os.path.getsize(OUT)} bytes')


if __name__ == '__main__':
    main()
