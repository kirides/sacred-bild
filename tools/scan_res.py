# Lists the instructions of the English Sacred.exe that use 1024x768-related constants (1024, 768, 512, 384, ...),
# the candidates gen_res_sites.py's site list was picked from. Writes them to .res_hits.json.
# Usage: python tools/scan_res.py
import sys, struct, collections, json
sys.path.insert(0, 'tools')
from eng import *
import capstone
from capstone import x86
md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md2.detail = True
code = IMG[TLO - BASE:THI - BASE]
hits = collections.defaultdict(list)
INTS = {0x400: '1024', 0x300: '768', 0x3ff: '1023', 0x2ff: '767', 0x200: '512', 0x180: '384'}
FLTS = {0x44800000: '1024.f', 0x44400000: '768.f', 0x44000000: '512.f', 0x43c00000: '384.f'}
off = 0
# linear sweep with resync on failure
while off < len(code):
    got = False
    for i in md2.disasm(code[off:off + 0x10000], TLO + off):
        got = True
        for op in i.operands:
            if op.type == x86.X86_OP_IMM:
                v = op.imm & 0xffffffff
                if v in INTS: hits[INTS[v]].append((i.address, f"{i.mnemonic} {i.op_str}"))
                if v in FLTS: hits[FLTS[v]].append((i.address, f"{i.mnemonic} {i.op_str}"))
        off = i.address + i.size - TLO
    if not got: off += 1
# float constants in data referenced via memory operands
for name, lst in hits.items():
    print(name, len(lst))
json.dump({k: v for k, v in hits.items()}, open('.res_hits.json', 'w'))
