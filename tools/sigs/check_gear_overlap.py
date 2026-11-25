# Checks that Sacred Gear and SacredBild never get in each other's way in sacred.exe, whichever of the two loads
# first. A signature window covers the bytes a module reads to find an address; a module that patches (hooks,
# writes) an address changes the bytes there. So:
# - no Gear window may overlap a window of an address SacredBild patches (SacredBild loaded first would hide the
#   bytes Gear's signature needs, or Gear would find code that SacredBild has replaced);
# - no Gear window of an address Gear patches may overlap any SacredBild window.
# Windows that only read the same bytes (both modules find g_pObjectManager the same way) are fine.
# Which addresses a module patches: the Addr names inside the argument list of its Patch::hook / write / value /
# imm32 / redirectCall calls, and, to be safe, any Addr name assigned on the five lines before such a call (an
# address put in a variable first). resolution_sites.inc lists patched sites only.
# Usage: python tools/sigs/check_gear_overlap.py   (SACRED_ENG as for gen_sigs.py)
import glob, os, re, sys
sys.path[:0] = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)) for p in [(), ('..',), ('..', 'sigs'), ('..', 'gearapi')]]  # tools/, tools/sigs, tools/gearapi
import gear_sites
import paths
import gen_sigs
import sigs

ROOT = gen_sigs.ROOT
ENTRY = re.compile(r'"((?:[0-9A-F?]{2} ?)+)"')
PATCH_CALL = re.compile(r'Patch::(?:hook|write|value|imm32|redirectCall)\s*(?:<[^>]*>)?\s*\(')
ADDR = re.compile(r'Addr::(\w+)')
ASSIGNED = re.compile(r'[^=!<>]=[^=]')   # an assignment, not a comparison


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


def patched_names(sources):
    """Addr names that the given C++ files patch (see the header)."""
    names = set()
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        lines = text.split('\n')
        for m in PATCH_CALL.finditer(text):
            end = text.find(';', m.end())
            names.update(ADDR.findall(text[m.start():end if end >= 0 else len(text)]))
            line = text.count('\n', 0, m.start())
            for before in lines[max(0, line - 5):line]:
                if ASSIGNED.search(before):
                    names.update(ADDR.findall(before))
    return names


def base(name):
    """'uiMouseReads[3]' -> 'uiMouseReads'."""
    return name.split('[')[0]


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG)
    gear = windows(paths.GEAR_SIGS, ref)
    bild_sigs = windows(paths.SACRED_SIGS, ref)
    bild_sites = windows(paths.RESOLUTION_SITES, ref)
    bild_patched = patched_names(sorted(set(glob.glob(os.path.join(paths.BILD, '**', '*.cpp'), recursive=True))
                                        | set(glob.glob(os.path.join(paths.CORE, '**', '*.cpp'), recursive=True))))
    gear_patched = patched_names(glob.glob(os.path.join(paths.GEAR, '**', '*.cpp'), recursive=True))

    bild_all = bild_sigs + bild_sites
    bild_patch_windows = [b for b in bild_sigs if base(b[0]) in bild_patched] + bild_sites
    clashes = []
    for g in gear:
        against = bild_all if base(g[0]) in gear_patched else bild_patch_windows
        for b in against:
            if g[1] < b[2] and b[1] < g[2]:
                clashes.append((g, b))
    # Gear's site hooks (tools/sigs/gear_sites.py) write into the middle of functions: no SacredBild window may touch them
    for hook, name, fn, site, length, prefix in gear_sites.SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + length:
                clashes.append(((f'site {hook} {site:08X}', site, site + length), b))
    for name, index, fnname, fn, site, op, addr, vanilla in gear_sites.CONSTANT_SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + 6:
                clashes.append(((f'constant site {name}[{index}] {site:08X}', site, site + 6), b))
    for kind, site in gear_sites.ABILITY_SITES:
        length = {'store': 10, 'add': 5, 'edx': 9}[kind]
        for b in bild_all:
            if site < b[2] and b[1] < site + length:
                clashes.append(((f'ability site {site:08X}', site, site + length), b))
    for site in gear_sites.DAMAGE_SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + 5:
                clashes.append(((f'damage site {site:08X}', site, site + 5), b))
    for site in gear_sites.EFFECT_SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + 6:
                clashes.append(((f'effect site {site:08X}', site, site + 6), b))
    for kind, site, _ in gear_sites.BONUS_SITES:
        length = 7 if kind == 'int8' else 5
        for b in bild_all:
            if site < b[2] and b[1] < site + length:
                clashes.append(((f'bonus site {site:08X}', site, site + length), b))
    for kind, site in gear_sites.SLOT_SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + 5:
                clashes.append(((f'slot site {site:08X}', site, site + 5), b))
    for name, index, fnname, fn, site in gear_sites.BRANCH_SITES:
        for b in bild_all:
            if site < b[2] and b[1] < site + 6 + gear_sites.BRANCH_EXTRA.get(site, 0):
                clashes.append(((f'branch site {name}[{index}] {site:08X}', site, site + 6), b))
    for b in bild_all:
        if gear_sites.KEY_SITE < b[2] and b[1] < gear_sites.KEY_SITE + 6:
            clashes.append((('key site', gear_sites.KEY_SITE, gear_sites.KEY_SITE + 6), b))
    for g, b in clashes:
        print(f'overlap: Gear {g[0]} {g[1]:08X}-{g[2]:08X} and SacredBild {b[0]} {b[1]:08X}-{b[2]:08X}')
    print(f'{len(gear)} Gear windows ({len(gear_patched)} patched names, {len(gear_sites.SITES) + len(gear_sites.CONSTANT_SITES) + len(gear_sites.ABILITY_SITES) + len(gear_sites.DAMAGE_SITES) + len(gear_sites.EFFECT_SITES) + len(gear_sites.BONUS_SITES) + len(gear_sites.SLOT_SITES) + len(gear_sites.BRANCH_SITES) + 1} site hooks), {len(bild_all)} SacredBild windows '
          f'({len(bild_patch_windows)} patched), {len(clashes)} overlaps')
    sys.exit(1 if clashes else 0)


if __name__ == '__main__':
    main()
