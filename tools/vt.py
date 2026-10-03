import pefile, struct, sys
EXE = r"B:\Spiele\GOG Games\Sacred Gold\sacred.exe"
pe = pefile.PE(EXE, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
def u32(va): return struct.unpack_from("<I", img, va - base)[0]
for a in sys.argv[1:]:
    va = int(a, 16); print(f"vtable {va:08x}")
    for i in range(80):
        f = u32(va + 4*i)
        if not (0x401000 <= f < 0x890000): break
        print(f"  [{i:2d}] +{4*i:03x} {f:08x}")
