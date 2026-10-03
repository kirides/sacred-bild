# Emits C++ patch-site tables for the resolution module (DE build), verifying each encoding.
import sys, struct
sys.path.insert(0, 'tools')
from de import *
from funcs import func_of
import capstone
from capstone import x86
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); cs.detail = True

def insn_at(a):
    i = find_insn_containing(a)
    assert i.address == a, (hex(a), hex(i.address))
    return i

def find_insn_containing(op_addr):
    # Linear sweep from the containing function's entry point keeps instruction boundaries aligned.
    start = func_of(op_addr)[0]
    for i in cs.disasm(rd(start, op_addr - start + 16), start):
        if i.address <= op_addr < i.address + i.size:
            return i
    raise Exception(hex(op_addr))

def mem_operand(op_addr, target):
    i = find_insn_containing(op_addr)
    assert any(o.type == x86.X86_OP_MEM and (o.mem.disp & 0xffffffff) == target for o in i.operands), (hex(op_addr), i.mnemonic, i.op_str)
    off = i.disp_offset
    assert i.address + off == op_addr, (hex(op_addr), hex(i.address), off)
    return i

def imm_operand(insn_addr, expected):
    i = insn_at(insn_addr)
    imms = [o.imm & 0xffffffff for o in i.operands if o.type == x86.X86_OP_IMM]
    assert expected in imms, (hex(insn_addr), i.mnemonic, i.op_str)
    return i, insn_addr + i.imm_offset

def f2u(f): return struct.unpack('<I', struct.pack('<f', f))[0]

out = []
# World-side memory operands: 1024.0f -> screen width, 768.0f -> screen height
W1024, H768 = 0x88e6f4, 0x890040
# Not FUN_00623c40 (0x623c84/0x623c9e): with FUN_00623a20 and g_unzoomedProjection it converts between world
# units and pixel offsets, which must stay independent of the screen size (game logic uses it everywhere).
mem_w = [0x61e5c4, 0x624af4, 0x628f67, 0x629296, 0x62b43c, 0x62dc34, 0x62ea2d, 0x62f30f, 0x630658, 0x630f41]
mem_h = [0x61e5ee, 0x624b05, 0x628f91, 0x6292be, 0x62ea60, 0x62f347, 0x63068c, 0x630f79]
for a in mem_w:
    i = mem_operand(a, W1024); out.append(('MemW', a, W1024, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
for a in mem_h:
    i = mem_operand(a, H768); out.append(('MemH', a, H768, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
# Zoomed projection doubles: world view, zoom setters, and the character preview (0x4b1740),
# which renders at screen center and reads back a fixed-size pixel region.
proj = {0x88ee40: 'ProjNegW', 0x88ee48: 'ProjPosW', 0x88ee50: 'ProjNegH', 0x88ee58: 'ProjPosH'}
proj_sites = {0x88ee40: [0x625011, 0x62838b, 0x628565, 0x628d13, 0x4b1a00], 0x88ee48: [0x625003, 0x62837d, 0x628557, 0x628d02, 0x4b19ef],
              0x88ee50: [0x624ff5, 0x62836f, 0x628549, 0x628cf1, 0x4b19de], 0x88ee58: [0x624fdd, 0x628357, 0x628531, 0x628cd9, 0x4b19c3]}
for tgt, sites in proj_sites.items():
    for a in sites:
        i = mem_operand(a, tgt); out.append((proj[tgt], a, tgt, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
# Unzoomed ortho pushes (float immediates)
# Only the device projections set by cEngine_ctor and 0x60d7e0. The same pushes in 0x624f10, 0x628210, 0x6283f0
# and 0x6289d0 build g_unzoomedProjection (0x182ccf0), the world <-> pixel conversion matrix: leave it alone.
pushes = {f2u(267.0): ('ImmPosW', [0x60c201, 0x60da5e]),
          f2u(-267.0): ('ImmNegW', [0x60c206, 0x60da63]),
          f2u(200.0): ('ImmPosH', [0x60c1f7, 0x60da54]),
          f2u(-200.0): ('ImmNegH', [0x60c1fc, 0x60da59])}
for val, (kind, sites) in pushes.items():
    for a in sites:
        i, op = imm_operand(a, val); out.append((kind, op, val, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
def scan_imm(lo, hi, val):
    res = []
    for i in cs.disasm(rd(lo, hi - lo), lo):
        if any(o.type == x86.X86_OP_IMM and (o.imm & 0xffffffff) == val for o in i.operands):
            res.append(i)
    return res
# Full-screen overlay quad drawn by the render thread outside the UI (FUN_0060e100)
for i in scan_imm(0x60e100, 0x60e340, f2u(1024.0)):
    out.append(('ImmFW', i.address + i.imm_offset, f2u(1024.0), f"{i.mnemonic} {i.op_str} [overlay]"))
for i in scan_imm(0x60e100, 0x60e340, f2u(768.0)):
    out.append(('ImmFH', i.address + i.imm_offset, f2u(768.0), f"{i.mnemonic} {i.op_str} [overlay]"))
# World/effect positions computed relative to the 1024x768 screen center: ftol(...) + 512 / + 384.
# View space is centered on (W/2, H/2) once the view size is patched, so every center constant in these
# passes has to follow: the int adds below and the float 512.0/384.0 operands of the same formulas,
# screen = (v - 512.0) * zoom + 512 and the culling inverse v = (p - 512.0) / zoom + 512.0.
# Also: "center - camera" loads (mov reg, 512 / 384) and the inverse "pos - center" (sub) in 0x41eaf0.
center_x = [0x41b25c, 0x41b3f6, 0x41b4d6, 0x41e560, 0x62b4c6, 0x62dcc6, 0x62e398, 0x62f967, 0x6315aa,
            0x41b184, 0x41eb07, 0x62cc40, 0x62fe60, 0x631aca, 0x631c53]
center_y = [0x41b27e, 0x41b3e4, 0x41b4c4, 0x41e596, 0x62b4e9, 0x62dce1, 0x62e3a9, 0x62f96d, 0x6315b0,
            0x41b1a1, 0x41eaf7, 0x62cc50, 0x62fe7d, 0x631ae7, 0x631c70]
# FUN_006285c0 converts the camera to world units as 2 * cam / (backbuffer width * unzoomedProj._11) for the
# conversion matrix (0x182cd40): scale the matrix entries by 1024 / W, 768 / H so the product stays 1024-based.
UNZ11, UNZ22 = 0x182ccf0, 0x182cd04
for a, tgt, kind in ((0x6288de, UNZ11, 'MemUnzX'), (0x6288fe, UNZ22, 'MemUnzY')):
    i = mem_operand(a, tgt); out.append((kind, a, tgt, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
# Object/creature passes cull their screen position against 1024 + 200 / 768 + 200.
for a in (0x62f988, 0x6315cb):
    i, op = imm_operand(a, 0x4c8); out.append(('ImmCullW', op, 0x4c8, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
for a in (0x62f9a7, 0x6315ea):
    i, op = imm_operand(a, 0x3c8); out.append(('ImmCullH', op, 0x3c8, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
F512, F384, F818, F888 = 0x88e6fc, 0x88e9f0, 0x89011c, 0x890124
world_center_funcs = {0x41eaf0, 0x62b000, 0x62d530, 0x62de70, 0x62e580, 0x6300d0, 0x632020, 0x6252c0}
for tgt, kind in ((F512, 'MemHalfW'), (F384, 'MemHalfH')):
    for a in xrefs(tgt):
        if func_of(a)[0] in world_center_funcs:
            i = mem_operand(a, tgt); out.append((kind, a, tgt, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
# Ground-tile culling: view-space y < 818 (768 + 50) in cWorldView_renderTileRow, < 888 (768 + 120) in 0x62d530
for a in xrefs(F818):
    i = mem_operand(a, F818); out.append(('MemCullH', a, F818, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
for a in xrefs(F888):
    i = mem_operand(a, F888); out.append(('MemCullH2', a, F888, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
for a in center_x:
    i, op = imm_operand(a, 0x200); out.append(('ImmHalfW', op, 0x200, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
for a in center_y:
    i, op = imm_operand(a, 0x180); out.append(('ImmHalfH', op, 0x180, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))
# Overhead-label layout rect: FUN_00662300(rect, 1024, 768)
for a, v in ((0x60e940, 0x300), (0x60e945, 0x400), (0x6272f8, 0x300), (0x6272fd, 0x400)):
    i, op = imm_operand(a, v); out.append(('ImmIW' if v == 0x400 else 'ImmIH', op, v, f"{i.mnemonic} {i.op_str} [{func_of(a)[1]}]"))

print("// Generated by tools/gen_res_sites.py -- do not edit by hand.")
print("static const Site kResolutionSites[] = {")
for kind, a, exp, note in out:
    print(f"    {{Kind::{kind}, 0x{a:08X}, 0x{exp:08X}}}, // {note}")
print("};")
