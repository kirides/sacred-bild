# Prints the ret sizes of functions of the English exe: a hook must declare ret/4 stack arguments (check_hooks.py checks the
# hooked ones).
# Usage: python tools/check_rets.py <address>...
import sys, bisect
sys.path.insert(0, 'tools')
from eng import *
from funcs import _A
for a in (int(x, 16) for x in sys.argv[1:]):
    i = bisect.bisect_right(_A, a)
    end = _A[i] if i < len(_A) else a + 0x4000
    rets = sorted({ins.op_str or '0' for ins in md.disasm(rd(a, end - a), a) if ins.mnemonic == 'ret'})
    print(f"{a:08x} rets={rets}")
