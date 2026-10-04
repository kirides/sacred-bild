# Prints the slots of vtables in the English Sacred.exe (how the virtual function offsets in sacred_addr.h were found).
# Usage: python tools/vt.py <vtable address>...
import pefile, struct, sys
from gen_sigs import SACRED_ENG as EXE
pe = pefile.PE(EXE, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tlo, thi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
def u32(va): return struct.unpack_from("<I", img, va - base)[0]
for a in sys.argv[1:]:
    va = int(a, 16); print(f"vtable {va:08x}")
    for i in range(80):
        f = u32(va + 4*i)
        if not (tlo <= f < thi): break   # the slots end where the pointers stop pointing into code
        print(f"  [{i:2d}] +{4*i:03x} {f:08x}")
