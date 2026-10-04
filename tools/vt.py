# Prints the slots of vtables in the DE sacred.exe (how the virtual function offsets in sacred_addr.h were found).
# Usage: python tools/vt.py <vtable address>...
import pefile, struct, sys
from gen_sigs import SACRED_DE as EXE
pe = pefile.PE(EXE, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
def u32(va): return struct.unpack_from("<I", img, va - base)[0]
for a in sys.argv[1:]:
    va = int(a, 16); print(f"vtable {va:08x}")
    for i in range(80):
        f = u32(va + 4*i)
        if not (0x401000 <= f < 0x890000): break
        print(f"  [{i:2d}] +{4*i:03x} {f:08x}")
