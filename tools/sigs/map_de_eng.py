# German -> English address map of Sacred.exe: functions by masked-byte matching, globals by pairing the absolute
# operands of matched instructions. General knowledge about the two builds (not ReBorn specific): a patch written
# against German code (ReBorn's) is translated to the English exe through it.
#
#   python tools/sigs/map_de_eng.py [--de sacred.exe] [--eng Sacred.exe] [--out tools/sigs/data/map_de_eng.json]
#
# Output (tools/sigs/data/map_de_eng.json):
#   functions: [[de, eng, length, score]]   score = share of equal masked bytes (1.0 = identical apart from addresses)
#   globals:   {"de": [eng, votes]}         data addresses and call targets paired from matched instructions
#   stats:     counts
import argparse, bisect, json, os, sys
sys.path[:0] = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)) for p in [(), ('..',), ('..', 'sigs'), ('..', 'gearapi')]]  # tools/, tools/sigs, tools/gearapi
import capstone
import sigs
import paths
from funcs import _A

ROOT = paths.ROOT
ENG = os.environ.get('SACRED_ENG', r'B:\Spiele\GOG Games\sacred gold GOG\Sacred.exe')
DE = os.environ.get('SACRED_DE', r'B:\Spiele\GOG Games\Sacred Gold\sacred.exe')


def masked_function(img, va, end):
    """(masked bytes of the function's instructions, [(insn address, insn)]) up to end, trailing padding dropped."""
    insns = [i for i in sigs.cs.disasm(img.rd(va, end - va), va)]
    while insns and insns[-1].bytes[0] in (0xCC, 0x90) and len(insns[-1].bytes) == 1:
        insns.pop()
    flat = []
    for i in insns:
        flat.extend(sigs.masked(img, i))
    return flat, insns


def score(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    same = sum(1 for x, y in zip(a, b) if x is None or y is None or x == y)
    return same / max(len(a), len(b))


def find_function(de, flat, pred, window, ninsn):
    """Best DE start for a masked function near `pred` (needle = its first concrete bytes)."""
    needle = bytearray()
    for b in flat:
        if b is None:
            break
        needle.append(b)
    if len(needle) < 3:
        # starts with a wildcard (a call): use the first concrete run after it
        run = bytearray()
        for b in flat[:48]:
            if b is None:
                if len(run) >= 6:
                    break
                run = bytearray()
            else:
                run.append(b)
        if len(run) < 3:
            return None, 0.0
        off = flat.index(run[0])  # approximate offset of the run (first occurrence of that byte)
        needle = run
    else:
        off = 0
    lo = max(de.tlo, pred - window)
    hi = min(de.thi, pred + window)
    text = de.text[lo - de.tlo:hi - de.tlo]
    best, best_score, best_dist = None, 0.0, 1 << 30
    pos = text.find(bytes(needle))
    while pos >= 0:
        start = lo + pos - off
        cand_insns = list(sigs.cs.disasm(de.rd(start, len(flat) + 64), start))[:ninsn]
        cand = [b for i in cand_insns for b in sigs.masked(de, i)]
        s = score(flat, cand) if len(cand) == len(flat) else score(flat, cand) * 0.95
        dist = abs(start - pred)
        if s > best_score + 1e-9 or (abs(s - best_score) <= 1e-9 and dist < best_dist):
            best, best_score, best_dist = start, s, dist
        pos = text.find(bytes(needle), pos + 1)
    return best, best_score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--de', default=DE)
    ap.add_argument('--eng', default=ENG)
    ap.add_argument('--out', default=os.path.join(paths.DATA, 'map_de_eng.json'))
    ap.add_argument('--min-score', type=float, default=0.9)
    args = ap.parse_args()
    eng = sigs.Image(args.eng, _A)
    de = sigs.Image(args.de)
    starts = [a for a in _A if eng.tlo <= a < eng.thi]
    funcs, drift = [], 0
    unmatched = []
    for k, va in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else eng.thi
        flat, insns = masked_function(eng, va, end)
        if len(flat) < 6:
            continue
        pred = va - drift
        cand, s = find_function(de, flat, pred, 0x4000, len(insns))
        if cand is None or s < args.min_score:
            unmatched.append(va)
            continue
        drift = va - cand
        funcs.append((cand, va, len(flat), round(s, 4)))
    print(f'functions: {len(funcs)} matched of {len(starts)}; {len(unmatched)} unmatched', file=sys.stderr)

    # globals / call targets from matched instructions: same instruction count and opcode sequence
    votes = {}
    for de_va, eng_va, n, s in funcs:
        ef, ei = masked_function(eng, eng_va, eng_va + n + 16)
        df, di = masked_function(de, de_va, de_va + n + 16)
        if len(ei) != len(di):
            continue
        for a, b in zip(ei, di):
            if a.bytes[0] != b.bytes[0] or len(a.bytes) != len(b.bytes):
                continue
            pairs = []
            if a.bytes[0] in (0xE8, 0xE9) and len(a.bytes) == 5:
                pairs.append((b.address + 5 + int.from_bytes(b.bytes[1:], 'little', signed=True),
                              a.address + 5 + int.from_bytes(a.bytes[1:], 'little', signed=True)))
            elif a.bytes[0] == 0x0F and len(a.bytes) == 6 and 0x80 <= a.bytes[1] <= 0x8F:
                pass    # conditional jumps stay inside the function
            else:
                for attr, size in (('disp', 'disp_size'), ('imm', 'imm_size')):
                    pass
                if a.disp_size == 4 and b.disp_size == 4 and a.disp_offset == b.disp_offset:
                    x, y = a.disp & 0xFFFFFFFF, b.disp & 0xFFFFFFFF
                    if eng.in_image(x) and de.in_image(y):
                        pairs.append((y, x))
                if a.imm_size == 4 and b.imm_size == 4 and a.imm_offset == b.imm_offset:
                    ia = [o.imm & 0xFFFFFFFF for o in a.operands if o.type == capstone.x86.X86_OP_IMM]
                    ib = [o.imm & 0xFFFFFFFF for o in b.operands if o.type == capstone.x86.X86_OP_IMM]
                    if ia and ib and eng.in_image(ia[-1]) and de.in_image(ib[-1]):
                        pairs.append((ib[-1], ia[-1]))
            for d, e in pairs:
                votes.setdefault(d, {}).setdefault(e, 0)
                votes[d][e] += 1
    glob, conflicts = {}, 0
    for d, c in votes.items():
        e, n = max(c.items(), key=lambda kv: kv[1])
        if len(c) > 1:
            conflicts += 1
        glob[d] = (e, n)
    print(f'globals/targets: {len(glob)} paired, {conflicts} with conflicting votes', file=sys.stderr)
    out = {
        'de': os.path.basename(args.de), 'eng': os.path.basename(args.eng),
        'de_timestamp': f'{de.timestamp:08X}', 'eng_timestamp': f'{eng.timestamp:08X}',
        'functions': [list(f) for f in funcs],
        'unmatched_eng': [f'{a:08X}' for a in unmatched],
        'globals': {f'{d:08X}': [f'{e:08X}', n] for d, (e, n) in sorted(glob.items())},
        'stats': {'functions_matched': len(funcs), 'functions_total': len(starts), 'globals': len(glob),
                  'conflicts': conflicts},
    }
    with open(args.out, 'w', newline='\n') as f:
        json.dump(out, f, separators=(',', ':'))
    print(json.dumps(out['stats']), file=sys.stderr)


if __name__ == '__main__':
    main()
