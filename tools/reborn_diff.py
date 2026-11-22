# Lists where SacredReborn.exe's code differs from the German sacred.exe it is made from (same layout, same addresses),
# sorted into the kinds of change that matter for SacredBild's patches. Usage:
#   python tools/reborn_diff.py <sacred.exe (German)> <SacredReborn.exe> [function address ...]
# With addresses (hex), only runs within 0x800 bytes after them are listed (a hooked function's body).
import sys
import pefile


def text(path):
    pe = pefile.PE(path, fast_load=True)
    s = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    return s.get_data(), pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress


def runs(a, b, base):
    out, i = [], 0
    while i < min(len(a), len(b)):
        if a[i] == b[i]:
            i += 1
            continue
        j = i
        while j < min(len(a), len(b)) and (a[j] != b[j] or a[j + 1:j + 3] != b[j + 1:j + 3]):
            j += 1
        out.append((base + i, i, j - i))
        i = j
    return out


def kind(a, b, off, n):
    # The 4 byte operand containing the change: what it was, what it is.
    for st in range(max(off - 3, 0), off + 1):
        x, y = int.from_bytes(a[st:st + 4], 'little'), int.from_bytes(b[st:st + 4], 'little')
        if st + 4 >= off + n and x != y:
            if 0x4e5000 <= x < 0x1500000 and 0x4e5000 <= y < 0x1500000:
                return 'global moved', hex(x), hex(y)
            if x < 0x4e5000 and y < 0x4e5000 and x > 0x100 and y > 0x100:
                return 'struct offset / immediate', hex(x), hex(y)
    if b[off] == 0xE9 or b[off - 1:off] == b'\xE9':
        return 'jump into ReBorn code', '', ''
    return 'other', a[off:off + n].hex(), b[off:off + n].hex()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, base = text(sys.argv[1])
    b, _ = text(sys.argv[2])
    spans = [int(x, 16) for x in sys.argv[3:]]
    for va, off, n in runs(a, b, base):
        if spans and not any(s <= va < s + 0x800 for s in spans):
            continue
        k = kind(a, b, off, n)
        print(f'{va:08x} {n:4} {k[0]:28} {k[1]} -> {k[2]}')


main()
