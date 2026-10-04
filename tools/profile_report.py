# Summarizes SacredBild-profile.txt: maps sampled addresses to DE function names (tools/data/functions_de.tsv) and
# SacredBild's own samples to its functions (from the linker map of the build that ran).
# usage: python tools/profile_report.py [path-to-SacredBild-profile.txt] [top-N] [path-to-ddraw.map]
import sys, os, re, bisect, collections
sys.path.insert(0, os.path.dirname(__file__))
from funcs import func_of

path = sys.argv[1] if len(sys.argv) > 1 else r"B:\Spiele\GOG Games\Sacred Gold\SacredBild-profile.txt"
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
map_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), '..', 'out', 'build',
                                                               'msvc-x86', 'RelWithDebInfo', 'ddraw.map')

def demangle(sym):
    """Rough MSVC demangling: ?name@Scope@Outer@@... -> Outer::Scope::name."""
    if not sym.startswith('?') or sym.startswith('??'):
        return sym[:100]
    parts = sym[1:].split('@@')[0].split('@')
    parts = [p for p in parts if p and not p.startswith('?A0x')]   # anonymous namespaces
    return '::'.join(reversed(parts))

def load_map(path):
    """RVA -> function name from an MSVC /MAP file."""
    if not os.path.exists(path):
        return None
    base, syms = 0x10000000, []
    for line in open(path, encoding='latin1'):
        m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            base = int(m.group(1), 16)
            continue
        m = re.match(r'\s*0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{8})\s+f\b', line)
        if m:
            syms.append((int(m.group(2), 16) - base, demangle(m.group(1))))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]

SYMS = load_map(map_path)

def own_name(rva):
    if not SYMS:
        return f"+{rva:08x}"
    i = bisect.bisect_right(SYMS[0], rva) - 1
    return SYMS[1][i] if i >= 0 else f"+{rva:08x}"

threads = []      # (tid, samples, sections)
cur = None
section = None
for line in open(path, encoding='latin1'):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    m = re.match(r'\[thread (\d+) samples (\d+)\]', line)
    if m:
        cur = {'tid': int(m.group(1)), 'samples': int(m.group(2)), 'exclusive': [], 'external': [], 'inclusive': [],
               'external_eip': [], 'external_caller': []}
        threads.append(cur)
        continue
    m = re.match(r'\[(\w+)\]', line)
    if m:
        section = m.group(1)
        continue
    parts = line.split()
    if section == 'external_caller':
        cur[section].append((parts[0], parts[1], int(parts[2], 16), int(parts[3])))
        continue
    if section in ('external', 'external_eip'):
        cur[section].append((parts[0], int(parts[1], 16), int(parts[2])))
    else:
        cur[section].append((int(parts[0], 16), int(parts[1])))

def fname(addr):
    start, name = func_of(addr)
    return f"{name} ({start:08x})"

for t in sorted(threads, key=lambda t: -t['samples']):
    n = max(t['samples'], 1)
    print(f"=== thread {t['tid']}: {t['samples']} samples ===")
    excl = collections.Counter()
    for addr, c in t['exclusive']:
        excl[fname(addr)] += c
    ext_mod = collections.Counter()
    ext_caller = collections.Counter()
    for mod, caller, c in t['external']:
        ext_mod[mod] += c
        ext_caller[(mod, fname(caller) if caller else '?')] += c
    total_exe = sum(excl.values())
    print(f"  in sacred.exe: {100 * total_exe / n:.1f}%   outside: {100 * (n - total_exe) / n:.1f}%")
    print("  -- outside sacred.exe by module")
    for mod, c in ext_mod.most_common(10):
        print(f"    {100 * c / n:5.1f}%  {mod}")
    print("  -- outside sacred.exe by calling game function")
    for (mod, caller), c in ext_caller.most_common(top):
        print(f"    {100 * c / n:5.1f}%  {mod:<20} <- {caller}")
    own = collections.Counter()
    for mod, rva, c in t['external_eip']:
        if mod == 'SacredBild':
            own[own_name(rva)] += c
    if own:
        print(f"  -- inside SacredBild by function{'' if SYMS else ' (no ddraw.map found)'}")
        for f, c in own.most_common(top):
            print(f"    {100 * c / n:5.1f}%  {f}")
    # Who called into each DLL: the first other non-system module on the stack.
    callers = collections.defaultdict(collections.Counter)
    for mod, caller, rva, c in t['external_caller']:
        if caller == 'SacredBild':
            where = f"SacredBild!{own_name(rva)}"
        elif caller.lower() == 'sacred.exe':
            start, name = func_of(0x400000 + rva)
            where = f"sacred.exe!{name}"
        else:
            where = caller
        callers[mod][where] += c
    if callers:
        print("  -- outside sacred.exe: module <- calling module/function")
        for mod, cnt in sorted(callers.items(), key=lambda kv: -sum(kv[1].values()))[:8]:
            for where, c in cnt.most_common(5):
                print(f"    {100 * c / n:5.1f}%  {mod:<18} <- {where}")
    print("  -- exclusive (own code) in sacred.exe")
    for f, c in excl.most_common(top):
        print(f"    {100 * c / n:5.1f}%  {f}")
    incl = collections.Counter()
    for addr, c in t['inclusive']:
        incl[fname(addr)] = max(incl[fname(addr)], c)      # several call sites of one function: keep the largest
    print("  -- inclusive (approximate, via return addresses on the stack)")
    for f, c in incl.most_common(top):
        print(f"    {100 * c / n:5.1f}%  {f}")
