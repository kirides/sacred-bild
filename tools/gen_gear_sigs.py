# Emits gear/game/gear_sigs.inc: the signatures that locate Sacred Gear's hooks and globals in sacred.exe, made the
# same way as SacredBild's (tools/gen_sigs.py: from the English GOG build, accepted only if they match exactly once;
# against the German build too if SACRED_DE points at it). Sacred Gear supports the English GOG build; the signatures
# still let it find the same code in other builds that have it.
# Usage: python tools/gen_gear_sigs.py   (SACRED_ENG / SACRED_DE as for gen_sigs.py)
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import gen_sigs
import gear_sites
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
    # Quick cast: a synthetic click event sent to the UI like ReBorn's SR_QUICK_CAST does (event class vtable, the object the engine hands
    # its events to, the cursor, the event handler).
    ('g_clickEventVtable', 0x00899248, 'data'),
    ('g_clickDownEventVtable', 0x0089704C, 'data'),
    ('g_uiEventTarget', 0x00AD6EF8, 'data'),
    ('g_cursorX', 0x0182EE18, 'data'),
    ('g_cursorY', 0x0182EE1C, 'data'),
    # SacredBild detours the handler's first bytes (cEngine_worldMouse): Gear finds it by a window 0x30 bytes inside and calls it from there.
    ('cWindow_handleEventInner', 0x006172F0, 'code'),
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
    # Item icons (gear/itemicon.cpp): what the inventory windows do to draw an item (cInventoryEntry_render 0x5DC430,
    # per item): load the item's model resources (0x640640, thiscall (item)), take the model state pointer
    # (0x44FC30) and draw the item's Granny model through the model renderer: its singleton (0x401660, cdecl ()) and
    # 0x40E9C0 (thiscall (renderer, device, model0, model1, x, y, type, scale, 0, flags, 0, 0), ret 0x2C).
    ('cItem_loadResources', 0x00640640, 'func'),
    ('modelRenderer_instance', 0x00401660, 'func'),
    ('modelRenderer_drawItem', 0x0040E9C0, 'func'),
    ('renderFlags_instance', 0x00643110, 'func'),
    ('renderFlags_set', 0x00643430, 'func'),
    # Mod UI input (gear/ui.cpp): cUI_Manager::receiveEvent (vtable slot 0, thiscall (manager, cEvent*) -> bool,
    # ret 4; cEngine_receiveEvent hands it every mouse and key event before the world) and the two mouse button
    # event classes (button at +4, x / y at +8 / +0xC in UI layout coordinates).
    ('cUI_Manager_receiveEvent', 0x00757840, 'func'),
    ('cEventMouseDown_vtable', 0x0089704C, 'data'),
    ('cEventMouseUp_vtable', 0x00899248, 'data'),
    # Inventories (gear/inventory.cpp): the manager of the 32 containers (backpacks, the chest ...) and the calls
    # the engine's own inventory code uses to find room for an item, put it in, take it out, and to create and
    # destroy items.
    ('g_pInventory', 0x00AD4448, 'data'),
    ('cInventory_findSpace', 0x005DABA0, 'func'),
    ('cInventory_put', 0x005D9F00, 'func'),
    ('cInventory_remove', 0x005DA630, 'func'),
    ('cItemType_size', 0x00428D60, 'func'),
    ('cObjectManager_destroy', 0x005FBDB0, 'func'),
    ('cItem_dataHash', 0x0042BF20, 'func'),
    ('cItem_entriesInsert', 0x005C9230, 'func'),
    # Socketing: the engine's "insert upgrade" (thiscall (item, upgrade id, socket) -> bool, ret 8).
    ('cItem_insertUpgrade', 0x005BFF60, 'func'),
    # Item tooltips (gear/itemtip.cpp): the reserved popup of the UI manager and the calls the inventory's hover uses.
    ('cUI_Manager_getPopup', 0x0075B1E0, 'func'),
    ('cUI_Popup_addFlags', 0x006E8860, 'func'),
    # SacredBild detours the start of this one and its own signature window covers the first 0x1B bytes, so the
    # signature is taken further in (0x6E6ED6, `mov al, [esp + 0x1C]`): the function is that address minus 0x36.
    ('cUI_Popup_setTextBody', 0x006E6ED6, 'code'),
    ('cUI_Popup_show', 0x00732550, 'func'),
    ('cUI_Control_clearFlags', 0x007325C0, 'func'),
    ('cUI_Popup_setOwner', 0x006E81C0, 'func'),
    ('engine_alloc', 0x008321A0, 'func'),
    ('engine_free', 0x0041D3B0, 'func'),
    # An item's record set and announced to the peers (0x16C) in a network game.
    ('cItem_setRecord', 0x005C0810, 'func'),
    # The object factory: an unregistered, constructed object (scratch items for tooltips).
    ('cObjectFactory_create', 0x00451300, 'func'),
    # The model loader and the type's model name / description: an item's icon without an item object.
    ('modelLoader_instance', 0x004115D0, 'func'),
    ('modelLoader_load', 0x00412590, 'func'),
    ('cTypeManager_modelName', 0x00426250, 'func'),
    ('cTypeManager_modelDescription', 0x004262B0, 'func'),
    ('modelRenderer_free', 0x00403E10, 'func'),
    # cWeapon3D's network read / write (vtable slot 8): tells weapons from other items (their four extra dwords).
    ('cWeapon3D_netIO', 0x005CF2D0, 'func'),
    # Saving (gear/save.cpp). The savegame window's event (code 0xF, sub 2; its class is made by the factory 0x8080A0
    # as `*p = &vtable`, 0x24 bytes) goes to cEngine::receiveEvent (0x618090, vtable slot 0 of the engine): it records
    # the slot and sets bit 0x40 of the engine's flags (+0x54), which the render thread loop turns into the write of
    # Save\GAMEnn.PAK. The engine is called through its vtable: SacredBild hooks the function's first bytes.
    ('cSaveEvent_vtable', 0x00890B38, 'data'),
    # The ability tables (gear/tables.cpp): T1 (ids from 1000, 0x78-byte records) and T2 (ids from 1, 0x94-byte
    # records) are static arrays that 0x4D1AF0 / 0x4D2310 scan by id; the lookups load the first record and compare
    # against the end (0x952A38 and 0x955344 are the end of T1 and of T2). The text lookup (the text table's instance
    # and `get(id)`) that the tables' name ids go through.
    ('abilityTable1First', 0x0094FE28, 'data', 0x004D1AFA),
    ('abilityTable1End', 0x00952A38, 'data', 0x004D1B08),
    ('abilityTable2First', 0x00952ACC, 'data', 0x004D231A),
    ('abilityTable2End', 0x00955344, 'data', 0x004D232A),
    ('textResources_instance', 0x006725E0, 'func'),
    ('textResources_get', 0x00672C90, 'func'),
    # Realms (gear/realm.cpp). The network object (g_pNetPlayers, 0x7D7A70) holds the lobby fields: +0x38 an IP override,
    # +0x3C the port and +0x39E4 the host (a string of three pointers begin / end / capacity); the login and register
    # calls copy the host each time. The network menu runs 0x7D8560 (thiscall (net, flag), ret 4) when a network
    # screen opens: it reads Settings.cfg into the object with 0x7EDEA0 (thiscall (net)) and then makes the game
    # client's network and the lobby connection (0x7D2BD0 -> 0x7CCBE0), which read the port with 0x81E400 (thiscall (config) -> port) at 0x7CCD28. The object writes itself back to Settings.cfg with 0x7EE780 (thiscall (net), fastcall in the exe).
    # The lobby connection (0x182EBD0; +8 / +0x10 tell that it is connected) and the string's assign (thiscall
    # (string*, first, last, allocator*), ret 0xC) the engine itself copies NETWORK_LOBBY in with.
    ('g_pLobby', 0x0182EBD0, 'data'),
    ('cString_assign', 0x00415BD0, 'func'),
    ('cNet_openScreen', 0x007D8560, 'func'),
    ('cNet_loadSettings', 0x007EDEA0, 'func'),
    ('cNet_saveSettings', 0x007EE780, 'func'),
    ('lobbyPortCall', 0x007CCD28, 'code'),
    # The difficulty level of the running game (0 .. 4): the engine's own code indexes tables with it (ReBorn's appended code too).
    ('g_difficulty', 0x0182EE44, 'data'),
]
# The functions that hold site hooks (tools/gear_sites.py): found by signature, the site is at a fixed offset in them.
GEAR += [(name, va, 'func') for name, va in gear_sites.functions().items()]


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG, _A)
    others = gen_sigs.other_builds(gen_sigs.SACRED_DE)
    path = os.path.join(gen_sigs.ROOT, 'gear', 'game', 'gear_sigs.inc')
    _, failed = gen_sigs.emit(path, '', 'Gear::Addr', GEAR, ref, others, 'kGearSigs')
    with open(path, newline='\n') as f:
        text = f.read().replace('tools/gen_sigs.py', 'tools/gen_gear_sigs.py', 1)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    # gear/game/gear_sites.inc: the site table the C++ side walks
    lines = ['// Generated by tools/gen_gear_sigs.py from tools/gear_sites.py -- do not edit by hand.',
             'static const SiteSpec kSites[] = {']
    for hook, name, fn, site, length, prefix in gear_sites.SITES:
        lines.append(f'    {{"{hook}", &Gear::Addr::{name}, 0x{site - fn:X}, {length}, {prefix}}}, // {site:08X} in {fn:08X}')
    lines.append('};')
    lines.append('static const ConstSiteSpec kConstSites[] = {')
    for name, index, fnname, fn, site, op, addr, vanilla in gear_sites.CONSTANT_SITES:
        lines.append(f'    {{"{name}", {index}, &Gear::Addr::{fnname}, 0x{site - fn:X}, {1 if op == "fmul" else 0}, 0x{addr:08X}}}, // {site:08X} in {fn:08X}')
    lines.append('};')
    lines.append('static const AbilitySiteSpec kAbilitySites[] = {')
    for i, (kind, site) in enumerate(gear_sites.ABILITY_SITES):
        fnname, fn = gear_sites.ABILITY_FUNCTION
        code = {'store': 0, 'add': 1, 'edx': 2}[kind]
        lines.append(f'    {{{i}, &Gear::Addr::{fnname}, 0x{site - fn:X}, {code}}}, // {site:08X} {kind}')
    lines.append('};')
    lines.append('static const DamageSiteSpec kDamageSites[] = {')
    for i, site in enumerate(gear_sites.DAMAGE_SITES):
        fnname, fn = gear_sites.DAMAGE_FUNCTION
        lines.append(f'    {{{i}, &Gear::Addr::{fnname}, 0x{site - fn:X}}}, // {site:08X}')
    lines.append('};')
    lines.append('static const DamageSiteSpec kEffectSites[] = {')
    for i, site in enumerate(gear_sites.EFFECT_SITES):
        fnname, fn = gear_sites.EFFECT_FUNCTION
        lines.append(f'    {{{i}, &Gear::Addr::{fnname}, 0x{site - fn:X}}}, // {site:08X}')
    lines.append('};')
    lines.append(f'static const uintptr_t kEffectAiLow = 0x{gear_sites.EFFECT_AI_CALLERS[0]:X}, kEffectAiHigh = 0x{gear_sites.EFFECT_AI_CALLERS[1]:X};')
    lines.append('static const PushSiteSpec kBonusSites[] = {')
    for i, (kind, site, vanilla) in enumerate(gear_sites.BONUS_SITES):
        fnname, fn = gear_sites.BONUS_FUNCTION
        code = {'int8': 0, 'int32': 1, 'float': 2}[kind]
        lines.append(f'    {{{i}, &Gear::Addr::{fnname}, 0x{site - fn:X}, {code}, 0x{vanilla:X}u}}, // {site:08X} {kind}')
    lines.append('};')
    lines.append('static const AbilitySiteSpec kSlotSites[] = {')
    for i, (kind, site) in enumerate(gear_sites.SLOT_SITES):
        fnname, fn = gear_sites.SLOT_FUNCTION
        code = {'copy': 0, 'call': 1}[kind]
        lines.append(f'    {{{i}, &Gear::Addr::{fnname}, 0x{site - fn:X}, {code}}}, // {site:08X} {kind}')
    lines.append('};')
    lines.append('static const BranchSiteSpec kBranchSites[] = {')
    for name, index, fnname, fn, site in gear_sites.BRANCH_SITES:
        extra = gear_sites.BRANCH_EXTRA.get(site, 0)
        lines.append(f'    {{"{name}", {index}, &Gear::Addr::{fnname}, 0x{site - fn:X}, {extra}}}, // {site:08X} in {fn:08X}')
    lines.append('};')
    fnname, fn = gear_sites.SLOT_FUNCTION
    lines.append(f'static const uint32_t kKeySite = 0x{gear_sites.KEY_SITE - fn:X}, kKeyEnd = 0x{gear_sites.KEY_END - fn:X};')
    lines.append('static const uint32_t kKeyFull[5] = {' + ', '.join(f'0x{a - fn:X}' for a in gear_sites.KEY_FULL) + '};')
    lines.append('static const uint32_t kKeySelect[5] = {' + ', '.join(f'0x{a - fn:X}' for a in gear_sites.KEY_SELECT) + '};')
    with open(os.path.join(gen_sigs.ROOT, 'gear', 'game', 'gear_sites.inc'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    if failed:
        sys.exit(f'{len(failed)} entries without a signature')


if __name__ == '__main__':
    main()
