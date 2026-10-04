# Map addresses to the containing function of the English Sacred.exe (nearest preceding entry point).
# Data: tools/data/functions_eng.tsv, exported from the Ghidra program "/Sacred.exe (GOG)".
import bisect, os
_A, _N = [], []
with open(os.path.join(os.path.dirname(__file__), 'data', 'functions_eng.tsv')) as f:
    for line in f:
        if line.startswith('#'):
            continue
        addr, name = line.rstrip('\n').split('\t')
        _A.append(int(addr, 16))
        _N.append(name)

def func_of(addr):
    i = bisect.bisect_right(_A, addr) - 1
    return (_A[i], _N[i]) if i >= 0 else (0, '?')
