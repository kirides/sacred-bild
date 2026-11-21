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


# Creature AI sites (ENG 004D4E50, the AI step): where the engine stores the ability the creature will use in [creature + 0x120]. Gear
# answers them with the event `creature.ability` (creature type, vanilla value, index, address): the vanilla value when no mod does.
# kind 'store': `mov dword [esi+0x120], imm32` (10 bytes); 'add': `add eax, imm32` (5): the base of an id the code adds 0 or 1 to; 'edx': the
# block `neg edx; sbb edx,edx; xor ecx,ecx; add edx,0x32` (9) that leaves 0x31 or 0x32 in edx.
ABILITY_FUNCTION = ('abilityAiFn', 0x004D4E50)
ABILITY_SITES = [
    ('store', 0x004D534A),
    ('store', 0x004D56D5),
    ('store', 0x004D56F3),
    ('store', 0x004D5722),
    ('store', 0x004D5733),
    ('store', 0x004D5742),
    ('store', 0x004D5754),
    ('store', 0x004D5760),
    ('store', 0x004D5D77),
    ('store', 0x004D60ED),
    ('store', 0x004D7452),
    ('store', 0x004D79B6),
    ('store', 0x004D7B5E),
    ('store', 0x004D80AF),
    ('store', 0x004D8569),
    ('store', 0x004D88CC),
    ('store', 0x004D8E59),
    ('store', 0x004D9531),
    ('store', 0x004D973F),
    ('store', 0x004DA600),
    ('store', 0x004DA9C8),
    ('store', 0x004DAB35),
    ('store', 0x004DAED5),
    ('store', 0x004DB11B),
    ('add', 0x004D80DD),
    ('edx', 0x004D5E68),
]


# Damage site (ENG 0056B130, the damage resolution of a hit): the `call __ftol` that turns the final float damage into the integer the
# function returns and the engine applies (after it the code only zeroes it for immune creatures). Gear answers it with the event
# `damage`: a handler gets the damage, the victim's and the attacker's creature types and may return another damage (16 bit: 0 .. 65534).
DAMAGE_FUNCTION = ('siteFn56B130', 0x0056B130)
DAMAGE_SITES = [
    0x0056DAAB,
]


# Effect site (ENG 00562470, "add a value to the creature's effect entries, clamp to their maximum"): the store of the new sum into the entry,
# `fstp [ecx+0x12]; fld [ecx+0x12]`. Gear answers it with the event `effect.add`; the AI's own calls (EFFECT_AI_CALLERS, return addresses in
# 004DA04B .. 004DA23C) always keep the engine's behaviour (ReBorn keeps a vanilla copy of the function for them).
EFFECT_FUNCTION = ('effectAddFn', 0x00562470)
EFFECT_SITES = [
    0x0056256D,
]
EFFECT_AI_CALLERS = (0x004DA04B, 0x004DA23C)


# Bonus sites (ENG 00571710, the function that rolls and applies item bonuses): `push imm` of the arguments of its random rolls (the upper limit
# of a `rand(n)` or of a random float) and of a text id. Gear answers them with the event `bonus.constant` (index, vanilla value, address): kind
# 'int8' is `push imm8` (2 bytes), 'int32' `push imm32` (5), 'float' `push imm32` that holds a float (5).
BONUS_FUNCTION = ('bonusFn', 0x00571710)
BONUS_SITES = [
    ('int8', 0x00578BF0, 5),
    ('int8', 0x00578C7A, 0xF),
    ('int32', 0x00578C87, 0x482),
    ('int8', 0x00578CC4, 0xF),
    ('int8', 0x00578D07, 5),
    ('float', 0x00578D31, 0x3E19999A),
    ('float', 0x00578D41, 0x3E19999A),
    ('float', 0x00578D57, 0x3E19999A),
    ('float', 0x00578D6D, 0x3E19999A),
    ('float', 0x00578D96, 0x3E19999A),
]


# Skill slot sites (ENG 006E1BC0, the event handler of the skill slot window): a digit key chose a slot (a `call 006E1A20` that selects it).
# Gear tells the mods with the event `skillslot.select` (index) right after the selection ran. kind 'copy': the 5 byte instruction after the
# call (`mov byte [esp+0x13], 1`) is copied into the stub; 'call': the call itself (rel32) is re-made.
SLOT_FUNCTION = ('skillSlotFn', 0x006E1BC0)
SLOT_SITES = [
    ('copy', 0x006E21E7),
    ('copy', 0x006E2219),
    ('copy', 0x006E2247),
    ('copy', 0x006E2275),
    ('call', 0x006E229E),
]


# Branch sites (the four hit handlers): a conditional `jcc rel32` (6 bytes) whose outcome Gear lets the mods decide: the event
# `combat.branch` (name, whether the engine's own condition takes the jump, index) returns true (jump) or false (fall through). 'hit.condition'
# is `cmp word [ebp], 0x3FC; jne` of the combat art handler (the art id 0x3FC); 'hit.secondary' is the `je` after the creation of an effect
# object (types 0x34B, 0x360, 0x35C, 0x35E): taken when the object could not be made, the code after it adds the secondary effect.
BRANCH_SITES = [
    ('hit.condition', 0, 'siteFn5038F0', 0x005038F0, 0x00505230),
    ('hit.secondary', 0, 'siteFn5038F0', 0x005038F0, 0x00505395),
    ('hit.secondary', 1, 'siteFn506770', 0x00506770, 0x005074E7),
    ('hit.secondary', 2, 'siteFn50AC00', 0x0050AC00, 0x0050BA2A),
    ('hit.secondary', 3, 'levelFn50E6C0', 0x0050E6C0, 0x0050F27C),
    # `jcc rel8` (two bytes): the next instruction(s) (the last number: how many bytes) are copied into the stub with it
    ('ai.timer', 0, 'abilityAiFn', 0x004D4E50, 0x004D7068),         # cmp eax, 0xB7 (the creature type); jne
    ('ability.dispatch', 0, 'siteFn51D840', 0x0051D840, 0x0051DA04), # cmp eax, 0x28 (the ability id); jge
    ('color.clamp', 0, 'siteFnColor', 0x004DCB80, 0x004DEF0B),       # cmp word [esp+0x30], 0xFF; jbe
    # The rune exchange tab of the combo master window (cUI_MasterLotto, UI_TABPAGE_LOTTO): the engine shows the rune selector (the arrows
    # that choose which rune the exchange makes) only with four runes in the slots (`cmp dx, 4; jne` in the selector update 006D3FD0: taken
    # hides it), and enables the exchange button only with two or more (`cmp dx, 2; jb` in the tab's render 006D49A0: taken disables it).
    ('master.select', 0, 'masterSelectFn', 0x006D3FD0, 0x006D3FF4),  # cmp dx, 4; jne
    ('master.ready', 0, 'masterRenderFn', 0x006D49A0, 0x006D4C97),   # cmp dx, 2; jb
]
BRANCH_EXTRA = {0x004D7068: 8, 0x0051DA04: 3, 0x004DEF0B: 4, 0x006D3FF4: 6, 0x006D4C97: 6}


# Skill slot key routing (ENG 006E1BC0): the `jb 006E2819` after `cmp ax, 0x30` of the character key handler. Gear lets the mods route a character
# (event `skillslot.key`): to the "full" case of a slot (as the digit keys do: the page choice, the COMBINE_SLOTS check, then the selection), to its
# "select" half (the selection only), or nowhere. KEY_FULL / KEY_SELECT are the start of those blocks per slot (0 .. 4), KEY_END the end of the handler.
KEY_SITE = 0x006E2197
KEY_FULL = [0x006E21BF, 0x006E21F1, 0x006E2223, 0x006E2251, 0x006E227F]
KEY_SELECT = [0x006E21DC, 0x006E220E, 0x006E223C, 0x006E226A, 0x006E2298]
KEY_END = 0x006E2819


# Drop sites (gear/drops.cpp). The engine's creature loot function (ENG 00559BF0, thiscall (creature, forced type, rarity bonus, survival float,
# killer id), ret 0x10) draws the chance of every drop attempt, the category of the item and its rarity with the CRT `rand` (ENG 00849B40, a 15 bit
# LCG: `% 1000`, `% 10000` ...). Its calls of `rand` (offsets from the function start) are redirected so that a mod may swap the generator.
LOOT_FUNCTION = ('lootRollFn', 0x00559BF0, 10709)
LOOT_RAND_CALLS = [0xDE, 0xF0, 0x380, 0x7A1, 0x80D, 0x857, 0x902, 0x924, 0x942, 0x95C, 0x973, 0x9C0, 0x9E2, 0xA6D, 0xA86, 0xA9A, 0xB1A,
                   0xB2E, 0xB74, 0xB8F, 0xBAA, 0xDC1, 0x1008, 0x12AD, 0x155B, 0x15FA, 0x17B1, 0x1AE0, 0x1B3A, 0x1C6C, 0x1C87, 0x1CA5,
                   0x1CF2, 0x1D36, 0x1E44, 0x1E5F, 0x1E7D, 0x1EC1, 0x1EDE, 0x1EFF, 0x1F51, 0x1F91, 0x2033, 0x2050, 0x2071, 0x20BC,
                   0x20FC, 0x25AF, 0x25CB, 0x25F2, 0x260E, 0x262A]
# The rarity draws: a roll of 1 .. 10000 that is compared with the cumulative weights of the rarity table (ENG 00AD4438 .. 00AD443C, 8 byte entries of
# a dword id and a word weight): `range` is `call 00443B20` with (1, 10000) (cdecl), `mod` is `call rand; cdq; idiv 10000`. (kind, function name,
# function address, call address)
RARITY_CALLS = [
    ('range', 'lootRollFn', 0x00559BF0, 0x0055A0F5),
    ('range', 'lootRollFn', 0x00559BF0, 0x0055A1F8),
    ('range', 'rarityFn485C10', 0x00485C10, 0x00487DA8),
    ('mod', 'rarityFn4AC940', 0x004AC940, 0x004AD4A4),
    ('mod', 'rarityFn5D57C0', 0x005D57C0, 0x005D5B45),
]


def functions():
    seen = {}
    for _, name, va, _, _, _ in SITES:
        seen[name] = va
    for _, _, name, va, _, _, _, _ in CONSTANT_SITES:
        seen[name] = va
    seen[ABILITY_FUNCTION[0]] = ABILITY_FUNCTION[1]
    seen[EFFECT_FUNCTION[0]] = EFFECT_FUNCTION[1]
    seen[BONUS_FUNCTION[0]] = BONUS_FUNCTION[1]
    seen[SLOT_FUNCTION[0]] = SLOT_FUNCTION[1]
    for _, _, fnname, fn, _ in BRANCH_SITES:
        seen[fnname] = fn
    seen[LOOT_FUNCTION[0]] = LOOT_FUNCTION[1]
    for _, name, va, _ in RARITY_CALLS:
        seen[name] = va
    return seen
