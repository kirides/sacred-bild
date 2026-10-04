# Lists the vtables of the English Sacred.exe by class name, from its MSVC RTTI (how the *_vtable addresses and the
# class names in sacred_addr.h were found).
# Usage: python tools/rtti.py [class name regex]
import pefile, struct, re, sys
from gen_sigs import SACRED_ENG as EXE
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
def u32(va): return struct.unpack_from("<I", img, va - base)[0]
def cstr(va):
    o = va - base; e = img.index(b"\0", o); return img[o:e].decode("latin1")
# type descriptors: name at +8
tds = {}
for m in re.finditer(rb"\.\?AV[^\0]{1,200}\0", img):
    tds[base + m.start() - 8] = m.group()[:-1].decode("latin1")
# complete object locators: sig=0, offset, cdoffset, pTD, pCHD
cols = {}
for off in range(0, len(img) - 20, 4):
    sig, o, cd, td, chd = struct.unpack_from("<5I", img, off)
    if sig == 0 and td in tds and base <= chd < base + len(img):
        cols[base + off] = (tds[td], o)
# vtables: dword == COL addr, next dword points into .text
text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
tlo, thi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
res = []
for off in range(0, len(img) - 8, 4):
    v = struct.unpack_from("<I", img, off)[0]
    if v in cols:
        f = struct.unpack_from("<I", img, off + 4)[0]
        if tlo <= f < thi:
            res.append((cols[v][0], cols[v][1], base + off + 4))
filt = sys.argv[1] if len(sys.argv) > 1 else None
for name, o, vt in sorted(res):
    if filt and not re.search(filt, name): continue
    print(f"{vt:08x} off={o:<4x} {name}")
