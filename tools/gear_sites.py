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


# Constant sites: a single `fmul dword [abs]` (D8 0D) or `fld dword [abs]` (D9 05) of the engine that loads a pooled float
# constant; Gear answers it with a float from the event `combat.constant` (name, vanilla value, index), the vanilla value when no
# mod does.
# (name, index, function name, function address, site address, opcode, the pooled constant's address, its vanilla value)
CONSTANT_SITES = [
    # the four hit handlers multiply an effect's value by 0.2 before adding it to a creature (call 0x562470)
    ('hit.effect_scale', 0, 'siteFn5038F0', 0x005038F0, 0x00505439, 'fmul', 0x00890DA0, 0.2),
    ('hit.effect_scale', 1, 'siteFn506770', 0x00506770, 0x0050758F, 'fmul', 0x00890DA0, 0.2),
    ('hit.effect_scale', 2, 'siteFn50AC00', 0x0050AC00, 0x0050BACC, 'fmul', 0x00890DA0, 0.2),
    ('hit.effect_scale', 3, 'levelFn50E6C0', 0x0050E6C0, 0x0050F31D, 'fmul', 0x00890DA0, 0.2),
    # and compare a roll times 0.01 with a threshold (a chance in percent)
    ('hit.chance_scale', 0, 'siteFn5038F0', 0x005038F0, 0x00505624, 'fmul', 0x00890CB0, 0.01),
    ('hit.chance_scale', 1, 'siteFn506770', 0x00506770, 0x00507793, 'fmul', 0x00890CB0, 0.01),
    ('hit.chance_scale', 2, 'siteFn50AC00', 0x0050AC00, 0x0050BCB4, 'fmul', 0x00890CB0, 0.01),
    ('hit.chance_scale', 3, 'levelFn50E6C0', 0x0050E6C0, 0x0050F516, 'fmul', 0x00890CB0, 0.01),
    # the five factors one function (0056B130) loads for the five difficulty levels
    ('difficulty.factor', 0, 'siteFn56B130', 0x0056B130, 0x0056D0DB, 'fld', 0x0089066C, 1.0),
    ('difficulty.factor', 1, 'siteFn56B130', 0x0056B130, 0x0056D0F2, 'fld', 0x00890DF0, 1.34),
    ('difficulty.factor', 2, 'siteFn56B130', 0x0056B130, 0x0056D0FC, 'fld', 0x00890DEC, 1.67),
    ('difficulty.factor', 3, 'siteFn56B130', 0x0056B130, 0x0056D106, 'fld', 0x00890750, 2.0),
    ('difficulty.factor', 4, 'siteFn56B130', 0x0056B130, 0x0056D110, 'fld', 0x00890DE8, 2.34),
    # the 0.01 of 0056B130's damage scale (ReBorn divides it by a per difficulty constant: 1 1 1 2 4)
    ('difficulty.scale', 0, 'siteFn56B130', 0x0056B130, 0x00570956, 'fmul', 0x00890CB0, 0.01),
    # three 0.01 scales in 00559BF0
    ('scale.559BF0', 0, 'siteFn559BF0', 0x00559BF0, 0x00559F08, 'fmul', 0x00890CB0, 0.01),
    ('scale.559BF0', 1, 'siteFn559BF0', 0x00559BF0, 0x0055A149, 'fmul', 0x00890CB0, 0.01),
    ('scale.559BF0', 2, 'siteFn559BF0', 0x00559BF0, 0x0055A248, 'fmul', 0x00890CB0, 0.01),
]


def functions():
    seen = {}
    for _, name, va, _, _, _ in SITES:
        seen[name] = va
    for _, _, name, va, _, _, _, _ in CONSTANT_SITES:
        seen[name] = va
    return seen
