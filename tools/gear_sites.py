# Sacred Gear's site hooks: places inside a function of sacred.exe where Gear replaces a block of instructions by a jump to a
# stub that asks Lua (gear/sitehook.cpp). A site is found as a function (a signature, gear_sigs.inc) plus a fixed
# offset, and the block's bytes are verified before anything is written. This file is the one list: gen_gear_sigs.py makes the
# function signatures and gear/game/gear_sites.inc from it, and check_gear_overlap.py checks the sites against SacredBild.
#
# (hook, function name, function address in the English GOG build, site address, block length, prefix length)
# hook 'level.factor': `[fild level;] fld st(0); fmul; fadd; fxch; fmul; fadd; fdivp; call __ftol`, the level to a factor.
# The prefix is the leading `fild dword [esp+x]` that loads the level (4 bytes) when the block loads it itself, 0 when the
# level is already on the FPU stack.
SITES = [
    ('level.factor', 'levelFn51CC30', 0x0051CC30, 0x0051CC30, 39, 4),
    ('level.factor', 'levelFn50E6C0', 0x0050E6C0, 0x0051065A, 39, 4),
    ('level.factor', 'levelFn50E6C0', 0x0050E6C0, 0x005113C3, 39, 4),
    ('level.factor', 'levelFn5893B0', 0x005893B0, 0x0058E516, 35, 0),
    ('level.factor', 'levelFn5893B0', 0x005893B0, 0x0058E59A, 35, 0),
    ('level.factor', 'levelFn5893B0', 0x005893B0, 0x0058E611, 35, 0),
]


def functions():
    seen = {}
    for _, name, va, _, _, _ in SITES:
        seen[name] = va
    return seen
