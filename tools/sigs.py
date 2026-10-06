# Byte signatures for code locations in the reference build (English) that have to be found in other builds as well.
# A signature is a run of whole instructions around the location with build-specific bytes left open:
# absolute addresses (4-byte displacements and immediates that point into the image) and rel32 branch targets.
# It is accepted only if it matches exactly once in the code section of every reference build.
import bisect, re
import capstone, pefile

cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
cs.detail = True
# Without operand details: instruction boundaries only, many times faster.
cs_lite = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

MAX_INSNS = 40          # longest window tried, in instructions
MIN_CONCRETE = 12        # fixed bytes a signature needs at least


class Image:
    def __init__(self, path, funcs=None):
        pe = pefile.PE(path, fast_load=True)
        self.path = path
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.timestamp = pe.FILE_HEADER.TimeDateStamp
        self.img = pe.get_memory_mapped_image()
        text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
        self.tlo = self.base + text.VirtualAddress
        self.thi = self.tlo + text.Misc_VirtualSize
        self.text = self.img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
        self.funcs = funcs or []    # sorted function entry points, for instruction boundaries
        self._starts = {}           # function entry -> its instruction addresses

    def rd(self, va, n):
        return self.img[va - self.base:va - self.base + n]

    def u32(self, va):
        return int.from_bytes(self.rd(va, 4), 'little')

    def in_image(self, v):
        return self.base <= v < self.base + self.size

    def find(self, sig, limit=None):
        """Start addresses of the (overlapping) matches of sig in the code section, the first `limit` of them.

        Anchored on the longest run of fixed bytes: bytes.find skips through the section at memchr speed, and only
        its hits are compared in full."""
        run = best_len = best_off = 0
        for i, b in enumerate(sig):
            run = run + 1 if b is not None else 0
            if run > best_len:
                best_len, best_off = run, i - run + 1
        if best_len == 0:
            rx = re.compile(b'(?=' + b'.' * len(sig) + b')', re.DOTALL)
            hits = [self.tlo + m.start() for m in rx.finditer(self.text)]
            return hits[:limit] if limit else hits
        needle = bytes(sig[best_off:best_off + best_len])
        rest = [(i, b) for i, b in enumerate(sig) if b is not None and not best_off <= i < best_off + best_len]
        text, n, out = self.text, len(sig), []
        pos = text.find(needle)
        while pos >= 0:
            start = pos - best_off
            if start >= 0 and start + n <= len(text) and all(text[start + i] == b for i, b in rest):
                out.append(self.tlo + start)
                if limit and len(out) >= limit:
                    break
            pos = text.find(needle, pos + 1)
        return out

    def matches_at(self, sig, va):
        if va < self.tlo or va + len(sig) > self.thi:
            return False
        data = self.rd(va, len(sig))
        return all(b is None or b == d for b, d in zip(sig, data))

    def func_range(self, va):
        i = bisect.bisect_right(self.funcs, va) - 1
        if i < 0:
            return va, va + 0x100
        return self.funcs[i], self.funcs[i + 1] if i + 1 < len(self.funcs) else self.thi

    def insns_around(self, va, start=None):
        """Instructions of the function containing va (linear sweep from its entry, or from `start`)."""
        lo, hi = self.func_range(va) if start is None else (start, start + 0x2000)
        hi = max(hi, va + 0x40)
        out = []
        for i in cs.disasm(self.rd(lo, hi - lo), lo):
            out.append(i)
            if i.address >= hi:
                break
        return out

    def insn_starts(self, va):
        """Instruction addresses of the function containing va (as insns_around sweeps it), cached per function."""
        lo, hi = self.func_range(va)
        starts = self._starts.get(lo)
        if starts is None:
            hi = max(hi, va + 0x40)
            starts = []
            for addr, _, _, _ in cs_lite.disasm_lite(self.rd(lo, hi - lo), lo):
                starts.append(addr)
                if addr >= hi:
                    break
            self._starts[lo] = starts
        return starts


def masked(img, insn):
    """Instruction bytes with build-specific operands replaced by None."""
    b = list(insn.bytes)
    op = b[0]
    if op in (0xE8, 0xE9) and len(b) == 5:
        b[1:5] = [None] * 4
    elif op == 0x0F and len(b) == 6 and 0x80 <= b[1] <= 0x8F:
        b[2:6] = [None] * 4
    if insn.disp_size == 4 and img.in_image(insn.disp & 0xFFFFFFFF):
        b[insn.disp_offset:insn.disp_offset + 4] = [None] * 4
    if insn.imm_size == 4 and op not in (0xE8, 0xE9) and not (op == 0x0F and 0x80 <= b[1] <= 0x8F):
        imms = [o.imm & 0xFFFFFFFF for o in insn.operands if o.type == capstone.x86.X86_OP_IMM]
        if imms and img.in_image(imms[-1]):
            b[insn.imm_offset:insn.imm_offset + 4] = [None] * 4
    return b


def concrete(sig):
    return sum(b is not None for b in sig)


def make(ref, va, others, *, start=None, forward_only=False):
    """Signature for va (an address inside an instruction) in `ref`, unique in ref and in every image of `others`.

    Returns (pattern, offset, [address in each other image]) or None. `start` gives a sweep start when ref has no
    function table; then and with forward_only the window starts at va's instruction.
    """
    insns = ref.insns_around(va, start)
    k = next((n for n, i in enumerate(insns) if i.address <= va < i.address + i.size), None)
    if k is None:
        return None
    sigs = [masked(ref, i) for i in insns]
    lo_limit = k if (forward_only or start is not None) else 0
    best = None
    for size in range(1, MAX_INSNS + 1):
        for first in range(k, max(lo_limit, k - size + 1) - 1, -1):
            last = first + size - 1
            if last < k or last >= len(insns):
                continue
            sig = [b for s in sigs[first:last + 1] for b in s]
            if concrete(sig) < MIN_CONCRETE:
                continue
            anchor = insns[first].address
            hits = ref.find(sig, 2)
            if hits != [anchor]:
                continue
            found = []
            for o in others:
                h = o.find(sig, 2)
                if len(h) != 1:
                    break
                found.append(h[0] + (va - anchor))
            else:
                best = (sig, va - anchor, found)
                break
        if best:
            break
    return best


def fmt(sig):
    return ' '.join('??' if b is None else f'{b:02X}' for b in sig)


def operand_of(img, insn_va, target):
    """(instruction, operand address) for the instruction at insn_va that uses target as an address operand."""
    i = next(cs.disasm(img.rd(insn_va, 16), insn_va))
    for size, off, val in ((i.disp_size, i.disp_offset, i.disp), (i.imm_size, i.imm_offset, i.operands[-1].imm)):
        if size == 4 and (val & 0xFFFFFFFF) == target:
            return insn_va, insn_va + off
    raise ValueError(f'{insn_va:#x} does not use {target:#x}')


def xrefs(img, target, limit=None):
    """Instructions in img's code that use target as a 4-byte displacement or immediate (needs a function table), the
    first `limit` of them. Only the instruction holding each candidate operand is decoded in full; the boundaries
    come from a cached sweep of its function."""
    pat = target.to_bytes(4, 'little')
    out = []
    pos = img.text.find(pat)
    while pos >= 0 and (limit is None or len(out) < limit):
        op_va = img.tlo + pos
        starts = img.insn_starts(op_va)
        k = bisect.bisect_right(starts, op_va) - 1
        if k >= 0:
            i = next(cs.disasm(img.rd(starts[k], 16), starts[k]), None)
            if i and i.address <= op_va < i.address + i.size and (
                    (i.disp_size == 4 and i.address + i.disp_offset == op_va) or
                    (i.imm_size == 4 and i.address + i.imm_offset == op_va)):
                out.append((i.address, op_va))
        pos = img.text.find(pat, pos + 1)
    return out
