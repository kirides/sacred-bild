# Checks that Sacred Gear and SacredBild never patch the same bytes of sacred.exe in a way that breaks the other, whichever
# of the two loads first.
#
# Finding code is no concern: both search the exe file's code (Sig::resolve), which no module patches, so a hook of one
# never hides the bytes the other's signature reads. Patches are: a Detours hook writes a jump over the first bytes of a
# function; a byte patch (Patch::write / value / imm32 / redirectCall, SacredBild's resolution sites, Gear's site hooks)
# checks that the bytes it replaces are the expected ones and refuses otherwise. So:
# - two Detours hooks of the same function are fine: Detours chains them (the second one's trampoline runs the first);
# - any other overlap of the bytes the two patch is a clash: a byte patch next to or inside the other's hook or patch
#   finds other bytes than it expects (and leaves its feature off), or a hook copies a byte patch into its trampoline.
# Which addresses a module patches: the Addr names inside the argument list of its Patch:: calls, and the Addr names
# assigned on the five lines before such a call to a variable the call then uses (an address put in a variable first).
# A hook is taken to cover 8 bytes (Detours moves whole instructions, at least 5 bytes), a byte patch 8 bytes too.
# Usage: python tools/sigs/check_gear_overlap.py   (SACRED_ENG as for gen_sigs.py)
import glob, os, re, sys
sys.path[:0] = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)) for p in [(), ('..',), ('..', 'sigs'), ('..', 'gearapi')]]  # tools/, tools/sigs, tools/gearapi
import gear_sites
import paths
import gen_sigs
import gen_gear_sigs
import sigs

ENTRY = re.compile(r'"((?:[0-9A-F?]{2} ?)+)"')
PATCH_CALL = re.compile(r'Patch::(hook|write|value|imm32|redirectCall)\s*(?:<[^>]*>)?\s*\(')
ADDR = re.compile(r'Addr::(\w+)')
ASSIGN = re.compile(r'^\s*(?:[\w:<>,\s\*&]+\s)?(\w+)\s*=[^=]')   # `x = ...` or `T x = ...`: the variable
FOOTPRINT = 8


def site_windows(path, ref):
    """(name, first, end) of every site in SacredBild's resolution_sites.inc (each window is patched in place)."""
    out = []
    with open(path) as f:
        for line in f:
            m = ENTRY.search(line)
            if m:
                sig = [None if b == '??' else int(b, 16) for b in m.group(1).split()]
                for va in ref.find(sig, limit=2):
                    out.append((f'resolution site {va:08X}', va, va + len(sig), 'write'))
    return out


def patched_names(sources):
    """{Addr name: 'hook' | 'write'} that the given C++ files patch (see the header)."""
    names = {}
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        lines = text.split('\n')
        for m in PATCH_CALL.finditer(text):
            kind = 'hook' if m.group(1) == 'hook' else 'write'
            end = text.find(';', m.end())
            call = text[m.start():end if end >= 0 else len(text)]
            for n in ADDR.findall(call):
                if names.get(n) != 'write':
                    names[n] = kind
            line = text.count('\n', 0, m.start())
            for before in lines[max(0, line - 5):line]:
                a = ASSIGN.match(before)
                if a and re.search(r'\b%s\b' % re.escape(a.group(1)), call):
                    for n in ADDR.findall(before):
                        if names.get(n) != 'write':
                            names[n] = kind
    return names


def footprints(entries, patched):
    """(name, first, end, kind) of each patched entry, at its English address."""
    out = []
    for e in entries:
        name, va = e[0], e[1]
        if name in patched:
            out.append((name, va, va + FOOTPRINT, patched[name]))
    return out


def gear_site_footprints():
    out = []
    for hook, name, fn, site, length, prefix in gear_sites.SITES:
        out.append((f'site {hook}', site, site + length, 'write'))
    for name, index, fnname, fn, site, op, addr, vanilla in gear_sites.CONSTANT_SITES:
        out.append((f'constant site {name}[{index}]', site, site + 6, 'write'))
    for kind, site in gear_sites.ABILITY_SITES:
        out.append((f'ability site', site, site + {'store': 10, 'add': 5, 'edx': 9}[kind], 'write'))
    for site in gear_sites.DAMAGE_SITES:
        out.append(('damage site', site, site + 5, 'write'))
    for site in gear_sites.EFFECT_SITES:
        out.append(('effect site', site, site + 6, 'write'))
    for kind, site, _ in gear_sites.BONUS_SITES:
        out.append(('bonus site', site, site + (7 if kind == 'int8' else 5), 'write'))
    for kind, site in gear_sites.SLOT_SITES:
        out.append(('slot site', site, site + 5, 'write'))
    for name, index, fnname, fn, site in gear_sites.BRANCH_SITES:
        out.append((f'branch site {name}[{index}]', site, site + 6 + gear_sites.BRANCH_EXTRA.get(site, 0), 'write'))
    out.append(('key site', gear_sites.KEY_SITE, gear_sites.KEY_SITE + 6, 'write'))
    return out


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG)
    bild_patched = patched_names(sorted(set(glob.glob(os.path.join(paths.BILD, '**', '*.cpp'), recursive=True))
                                        | set(glob.glob(os.path.join(paths.CORE, '**', '*.cpp'), recursive=True))))
    gear_patched = patched_names(glob.glob(os.path.join(paths.GEAR, '**', '*.cpp'), recursive=True))
    bild = footprints(gen_sigs.SACRED, bild_patched) + site_windows(paths.RESOLUTION_SITES, ref)
    gear = footprints(gen_gear_sigs.GEAR, gear_patched) + gear_site_footprints()
    clashes = []
    for g in gear:
        for b in bild:
            if g[1] < b[2] and b[1] < g[2] and not (g[3] == b[3] == 'hook' and g[1] == b[1]):
                clashes.append((g, b))
    for g, b in clashes:
        print(f'clash: Gear {g[0]} ({g[3]}) {g[1]:08X}-{g[2]:08X} and SacredBild {b[0]} ({b[3]}) {b[1]:08X}-{b[2]:08X}')
    shared = sum(1 for g in gear for b in bild if g[3] == b[3] == 'hook' and g[1] == b[1])
    print(f'{len(gear)} Gear patches, {len(bild)} SacredBild patches, {shared} functions both hook (chained), {len(clashes)} clashes')
    sys.exit(1 if clashes else 0)


if __name__ == '__main__':
    main()
