# Summarizes SacredBild-profile.txt: maps sampled addresses to DE function names (from .funcs_de.json).
# usage: python tools/profile_report.py [path-to-SacredBild-profile.txt] [top-N]
import sys, os, re, collections
sys.path.insert(0, os.path.dirname(__file__))
from funcs import func_of

path = sys.argv[1] if len(sys.argv) > 1 else r"B:\Spiele\GOG Games\Sacred Gold\SacredBild-profile.txt"
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30

threads = []      # (tid, samples, sections)
cur = None
section = None
for line in open(path, encoding='latin1'):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    m = re.match(r'\[thread (\d+) samples (\d+)\]', line)
    if m:
        cur = {'tid': int(m.group(1)), 'samples': int(m.group(2)), 'exclusive': [], 'external': [], 'inclusive': []}
        threads.append(cur)
        continue
    m = re.match(r'\[(\w+)\]', line)
    if m:
        section = m.group(1)
        continue
    parts = line.split()
    if section == 'external':
        cur['external'].append((parts[0], int(parts[1], 16), int(parts[2])))
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
    print("  -- exclusive (own code) in sacred.exe")
    for f, c in excl.most_common(top):
        print(f"    {100 * c / n:5.1f}%  {f}")
    incl = collections.Counter()
    for addr, c in t['inclusive']:
        incl[fname(addr)] = max(incl[fname(addr)], c)      # several call sites of one function: keep the largest
    print("  -- inclusive (approximate, via return addresses on the stack)")
    for f, c in incl.most_common(top):
        print(f"    {100 * c / n:5.1f}%  {f}")
