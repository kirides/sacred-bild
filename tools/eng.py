# Helpers over the English Sacred.exe (SACRED_ENG, see gen_sigs.py), shared by the other tools.
# Run alone: python tools/eng.py <address> [count] disassembles from there.
import pefile, struct, re, capstone
from gen_sigs import SACRED_ENG as EXE
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.get_memory_mapped_image()
TEXT = [s for s in pe.sections if s.Name.startswith(b".text")][0]
TLO, THI = BASE + TEXT.VirtualAddress, BASE + TEXT.VirtualAddress + TEXT.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = False
def u32(va): return struct.unpack_from("<I", IMG, va - BASE)[0]
def rd(va, n): return IMG[va - BASE: va - BASE + n]
def findstr(s):
    b = s.encode("latin1") if isinstance(s, str) else s
    return [BASE + m.start() for m in re.finditer(re.escape(b), IMG)]
def xrefs(va):
    pat = struct.pack("<I", va); out = []
    o = TLO - BASE
    while True:
        o = IMG.find(pat, o, THI - BASE)
        if o < 0: break
        out.append(BASE + o); o += 1
    return out
def calls_to(target):
    out = []
    t = IMG[TLO - BASE:THI - BASE]
    for m in re.finditer(rb"\xe8", t):
        a = TLO + m.start()
        if a + 5 > THI: break
        rel = struct.unpack_from("<i", t, m.start() + 1)[0]
        if (a + 5 + rel) & 0xffffffff == target: out.append(a)
    return out
def dis(va, n=40, stop_ret=True):
    out = []
    for i in md.disasm(rd(va, n * 8), va):
        out.append(f"{i.address:08x}  {i.bytes.hex():<20} {i.mnemonic} {i.op_str}")
        if len(out) >= n or (stop_ret and i.mnemonic in ("ret", "retn")): break
    return "\n".join(out)
def func_start(va, maxback=0x4000):
    # walk back to a likely prologue preceded by int3/nop/ret padding
    for a in range(va, va - maxback, -1):
        b = rd(a - 1, 4)
        if b[0] in (0xCC, 0x90, 0xC3) and (rd(a, 1)[0] in (0x55, 0x83, 0x81, 0x6A, 0x64, 0x56, 0x53, 0x8B, 0x51, 0xA1, 0x57, 0xB8)) and a % 16 == 0:
            return a
    return None
if __name__ == "__main__":
    import sys
    print(dis(int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 40))
