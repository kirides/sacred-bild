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
    # Mod drawing (gear/ui.cpp), all inside cUI_Manager_render (0x758ED0, which SacredBild hooks): the popup pass
    # (thiscall (manager, device, flag), ret 8) for the "hud" layer; the in-game branch's call of the party arrows
    # (0x6D01F0, cdecl (device)) right after the world, before any window, for the "world" layer; and, after the
    # escape menu and the message box in every mode, `call 0x649BE0` (the font manager, for the menus' version
    # text) for the "top" layer. EBX holds the device throughout the render.
    ('cUI_Manager_renderPopups', 0x00758DC0, 'func'),
    ('uiRenderWorldCall', 0x0075934F, 'code'),
    ('uiRenderTopCall', 0x00759585, 'code'),
    ('g_pFontManager', 0x00CDCA70, 'data'),
    ('g_pTextureManager', 0x013E7838, 'data'),
    ('cTextureManager_get', 0x0065ED90, 'func'),
    # The UI images: 0x54-byte records from 0x9ECEB0 (this is their +0x28, the texture handle, as the loader
    # 0x75CC70 walks them).
    ('uiImageTextures', 0x009ECED8, 'data'),
    # Items and pickup (gear/world.cpp, gear/pickup.cpp). cItemBase's type descriptor for __RTDynamicCast;
    # cCreature::pickupItem (thiscall (creature, item id) -> 1 picked / 0 not, ret 4: the only code that moves a world
    # item to a creature; its callers ignore the result); the collect behaviour's filter (thiscall (creature,
    # cObject* item) -> bool, ret 4: which items it walks to and picks up); an item's label colour (thiscall (item,
    # record or 0) -> ARGB, ret 4).
    ('cItemBase_typeDescriptor', 0x0094EBC0, 'data'),
    ('cCreature_pickupItem', 0x00558080, 'func'),
    ('cCreature_collectFilter', 0x004EC330, 'func'),
    ('cItem_labelColor', 0x005C13C0, 'func'),
    # Mod UI input (gear/ui.cpp): cUI_Manager::receiveEvent (vtable slot 0, thiscall (manager, cEvent*) -> bool,
    # ret 4; cEngine_receiveEvent hands it every mouse and key event before the world) and the two mouse button
    # event classes (button at +4, x / y at +8 / +0xC in UI layout coordinates).
    ('cUI_Manager_receiveEvent', 0x00757840, 'func'),
    ('cEventMouseDown_vtable', 0x0089704C, 'data'),
    ('cEventMouseUp_vtable', 0x00899248, 'data'),
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
