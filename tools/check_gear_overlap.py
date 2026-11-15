# Checks that Sacred Gear and SacredBild never touch the same bytes of sacred.exe, whichever of the two loads first:
# every signature window of one must lie apart from every signature window of the other. A signature window covers
# the bytes its module reads to find an address and, for a hooked function, the prologue Detours overwrites, so
# windows that don't overlap mean neither module's patches can hide the other's code from its signature scan.
# Usage: python tools/check_gear_overlap.py   (SACRED_ENG as for gen_sigs.py)
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import gen_sigs
import sigs

ROOT = gen_sigs.ROOT
ENTRY = re.compile(r'"((?:[0-9A-F?]{2} ?)+)"')


def windows(path, ref):
    """(name, first, last) of every signature in a generated .inc file, located in the English build."""
    out = []
    with open(path) as f:
        for line in f:
            m = ENTRY.search(line)
            if not m:
                continue
            sig = [None if b == '??' else int(b, 16) for b in m.group(1).split()]
            name = line.split('"')[1] if line.strip().startswith('{"') else line.strip()[:60]
            for va in ref.find(sig, limit=2):
                out.append((name, va, va + len(sig)))
    return out


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG)
    gear = windows(os.path.join(ROOT, 'gear', 'game', 'gear_sigs.inc'), ref)
    bild = []
    for inc in ('sacred_sigs.inc', 'resolution_sites.inc'):
        bild += windows(os.path.join(ROOT, 'src', 'game', inc), ref)
    clashes = [(g, b) for g in gear for b in bild if g[1] < b[2] and b[1] < g[2]]
    for g, b in clashes:
        print(f'overlap: Gear {g[0]} {g[1]:08X}-{g[2]:08X} and SacredBild {b[0]} {b[1]:08X}-{b[2]:08X}')
    print(f'{len(gear)} Gear windows, {len(bild)} SacredBild windows, {len(clashes)} overlaps')
    sys.exit(1 if clashes else 0)


if __name__ == '__main__':
    main()
