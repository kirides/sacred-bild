# Emits gear/game/gear_sigs.inc: the signatures that locate Sacred Gear's hooks and globals in sacred.exe, made the
# same way as SacredBild's (tools/gen_sigs.py: from the English GOG build, accepted only if they match exactly once;
# against the German build too if SACRED_DE points at it). Sacred Gear supports the English GOG build; the signatures
# still let it find the same code in other builds that have it.
# Usage: python tools/gen_gear_sigs.py   (SACRED_ENG / SACRED_DE as for gen_sigs.py)
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import gen_sigs
import sigs
from funcs import _A

# (name, ENG address, kind) as in gen_sigs.py.
GEAR = [
    # The in-game loop (cEngine_renderThreadRun, 0x60E350) runs the game's per-frame update and then draws. It calls
    # this zoom update (thiscall (cEngine*, float dt), ret 4) once per frame after the world update and before the
    # world view renders, outside loading screens: Sacred Gear's frame tick.
    ('cEngine_updateZoom', 0x0060DD20, 'func'),
    # The object whose +0x563008 / +0x56300C vector holds Creature.pak's 0x56-byte records (0x7E0708 loads it into
    # ECX before calling the loader 0x4371C0 with "Pak\\Creature.pak").
    ('g_pTypeManager', 0x00AAB5E4, 'data'),
    # Objects (gear/world.cpp). SacredBild reads some of these too (never patches them): check_gear_overlap.py
    # allows windows that only read the same bytes.
    ('g_pObjectManager', 0x00AD5C40, 'data'),
    ('cObjectManager_hero', 0x00603E30, 'func'),
    ('cObjectManager_create', 0x005FBA40, 'func'),
    ('cObjectManager_pop', 0x005FCCB0, 'func'),
    ('rtDynamicCast', 0x0084A961, 'func'),
    ('cObject_typeDescriptor', 0x008EB648, 'data'),
    ('cCreature_typeDescriptor', 0x008EB660, 'data'),
    # Automation (gear/automation.cpp).
    ('g_pApp', 0x00CDCA1C, 'data'),
    ('g_pUiManager', 0x017EEBBC, 'data'),
    ('g_pNetPlayers', 0x0182EBE8, 'data'),
    ('cMouse_instance', 0x00654F60, 'func'),
]


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG, _A)
    others = gen_sigs.other_builds(gen_sigs.SACRED_DE)
    path = os.path.join(gen_sigs.ROOT, 'gear', 'game', 'gear_sigs.inc')
    _, failed = gen_sigs.emit(path, '', 'Gear::Addr', GEAR, ref, others, 'kGearSigs')
    with open(path, newline='\n') as f:
        text = f.read().replace('tools/gen_sigs.py', 'tools/gen_gear_sigs.py', 1)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    if failed:
        sys.exit(f'{len(failed)} entries without a signature')


if __name__ == '__main__':
    main()
