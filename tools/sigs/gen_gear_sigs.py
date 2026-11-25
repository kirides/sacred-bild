# Emits src/gear/engine/gear_sigs.inc: the signatures that locate Sacred Gear's hooks and globals in sacred.exe, made the
# same way as SacredBild's (tools/sigs/gen_sigs.py: from the English GOG build, accepted only if they match exactly once;
# against the German build too if SACRED_DE points at it). Sacred Gear supports the English GOG build; the signatures
# still let it find the same code in other builds that have it.
# Usage: python tools/sigs/gen_gear_sigs.py   (SACRED_ENG / SACRED_DE as for gen_sigs.py)
import os, sys
sys.path[:0] = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)) for p in [(), ('..',), ('..', 'sigs'), ('..', 'gearapi')]]  # tools/, tools/sigs, tools/gearapi
import gen_sigs
import gear_sites
import paths
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
    # Objects (src/gear/world/world.cpp). SacredBild reads some of these too (never patches them).
    ('g_pObjectManager', 0x00AD5C40, 'data'),
    ('cObjectManager_hero', 0x00603E30, 'func'),
    ('cObjectManager_create', 0x005FBA40, 'func'),
    ('cObjectManager_pop', 0x005FCCB0, 'func'),
    # cObjectManager::unlink (thiscall (id), ret 4): takes an object out of its sector's list and marks it out of the world
    # (+0x14 bit 2); pop places only such objects, so a teleport of a creature in the world unlinks it first, as the
    # engine does before it destroys one (0x5FB530).
    ('cObjectManager_unlink', 0x005FC8B0, 'func'),
    ('rtDynamicCast', 0x0084A961, 'func'),
    ('cObject_typeDescriptor', 0x008EB648, 'data'),
    ('cCreature_typeDescriptor', 0x008EB660, 'data'),
    # Automation (src/gear/automation.cpp).
    ('g_pApp', 0x00CDCA1C, 'data', 0x00401CF7),   # not the reference in cGranny_renderShadow, which SacredBild detours
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
    # Mod drawing (src/gear/ui/ui.cpp), all inside cUI_Manager_render (0x758ED0, which SacredBild hooks): the popup pass
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
    # Items and pickup (src/gear/world/world.cpp, src/gear/items/pickup.cpp). cItemBase's type descriptor for __RTDynamicCast;
    # cCreature::pickupItem (thiscall (creature, item id) -> 1 picked / 0 not, ret 4: the only code that moves a world
    # item to a creature; its callers ignore the result); the collect behaviour's filter (thiscall (creature,
    # cObject* item) -> bool, ret 4: which items it walks to and picks up); an item's label colour (thiscall (item,
    # record or 0) -> ARGB, ret 4).
    ('cItemBase_typeDescriptor', 0x0094EBC0, 'data'),
    ('cCreature_pickupItem', 0x00558080, 'func'),
    ('cCreature_collectFilter', 0x004EC330, 'func'),
    ('cItem_labelColor', 0x005C13C0, 'func'),
    # Item icons (src/gear/items/itemicon.cpp): what the inventory windows do to draw an item (cInventoryEntry_render 0x5DC430,
    # per item): load the item's model resources (0x640640, thiscall (item)), take the model state pointer
    # (0x44FC30) and draw the item's Granny model through the model renderer: its singleton (0x401660, cdecl ()) and
    # 0x40E9C0 (thiscall (renderer, device, model0, model1, x, y, type, scale, 0, flags, 0, 0), ret 0x2C).
    ('cItem_loadResources', 0x00640640, 'func'),
    # Net trace (src/gear/netmsg.cpp): the client's send queue (thiscall (cGCclass* net, message*, flush), ret 8): a
    # message is a 16-byte header (u32 size | slot << 22, +0xE u16 message id) and its payload.
    ('cNet_queueMessage', 0x007D4860, 'func'),
    # The mod channel (src/gear/netmsg.cpp): build + queue a game message (thiscall (net, data, size, u16 type, flush), ret
    # 0x10), the connection object it takes (0182EBF0, what the chat sender 0x7D4510 is called with), and the
    # receive dispatcher's type switch (0x7DC260: `cmp ecx, 0x1D4; ja <next message>; ... jmp [edx*4 + table]`), whose
    # chat case (types 3, 4, 5: 0x7E0880) Gear routes through its own stub to drop its frames before the chat shows them.
    ('cNet_sendMessage', 0x007D4300, 'func'),
    ('g_pNet', 0x0182EBF0, 'data'),
    ('netDispatchSwitch', 0x007DCF91, 'code'),
    # Chat lines (src/gear/chat.cpp): cUI_TextLines::add (thiscall (lines, channel, font, wstring*, color), ret 0x10), the
    # message list every chat, server and game message line goes to (the UI manager's at +0x14C, 0x75A770).
    ('cUI_TextLines_add', 0x007237D0, 'func'),
    # Gold (src/gear/world/world.cpp): the hero stats' gold setter (thiscall (creature + 0x3A8, gold), ret 4), as merchants use it;
    # it keeps the anti-cheat copy in step and marks the values for the next 0x11B update, also in network games.
    # No signature of its own (its body is nearly addGold's): found through a call (0x461B74: lea ecx, [ebp + 0x3A8]; fstp; call).
    ('cStats_setGoldCall', 0x00461B74, 'code'),
    # The hero file writer ("SAVEHERO", cdecl bool (path), the network game's Save\HeroNN.pax every 300 s, on leaving and
    # at merchants): Gear raises game.hero_saved after it.
    ('heroSave', 0x0061CD00, 'func'),
    # cCommand_armaPlay::execute (fastcall (command), the game's own load / new game command): +4 the kind (1 load a savegame,
    # 4 load a network hero file, else "init", a new game), +8 the number; it posts WM_APP + 2 to the game window. Gear raises
    # savegame.load / savegame.new from it (gear.savegame, per savegame mod storage).
    ('armaPlayExecute', 0x007561E0, 'func'),
    # "LOADHERO" (cdecl (path, hero id, 0)): reads a hero file into the hero object with that id; a developer command
    # (0x614230) creates hero 2 and loads Save\Hero%2d.pax into it so. gear.party imports heroes with it.
    ('heroLoad', 0x0061D0F0, 'func'),
    # A game-wide byte the hero loader sets from the file's header (record 0xC3; read by the save writer, the mouse
    # handler, the stats code): an imported companion must not change it for the running game.
    ('g_heroFileFlag', 0x0182EE50, 'data'),
    # The backpack tab of the inventory window (cUI_InventoryTab, made by 0x6BFFE0): it keeps the hero id whose
    # container it shows (+0x154, +0x158) from when the UI was built; a control switch points it at the new hero.
    ('cUI_InventoryTab_vtable', 0x008965C4, 'data'),
    # The taskbar (cUI_Taskbar2, the UI manager's first in-game window) shows one hero's weapon slots, combat art slots,
    # belt and level-dependent slot count; setHero (thiscall (taskbar, hero id, 0), ret 8) is what its constructor
    # (0x6DF590) calls with the local id. A control switch calls it with the new hero.
    ('cUI_Taskbar2_vtable', 0x00896AC0, 'data'),
    ('cUI_Taskbar2_setHero', 0x006E0070, 'func'),
    # Hero names (src/gear/netmsg.cpp): a hero keeps its name as a text resource whose id is at creature +0x3C (0x26FF + the
    # hero's id); the text manager (singleton at 017E7F68, getter 0x6725E0) returns it: thiscall (manager, id) -> wstring*.
    ('g_pTextManager', 0x017E7F68, 'data'),
    ('cTextManager_get', 0x00672C90, 'func'),
    ('modelRenderer_instance', 0x00401660, 'func'),
    ('modelRenderer_drawItem', 0x0040E9C0, 'func'),
    ('renderFlags_instance', 0x00643110, 'func'),
    ('renderFlags_set', 0x00643430, 'func'),
    # Mod UI input (src/gear/ui/ui.cpp): cUI_Manager::receiveEvent (vtable slot 0, thiscall (manager, cEvent*) -> bool,
    # ret 4; cEngine_receiveEvent hands it every mouse and key event before the world) and the two mouse button
    # event classes (button at +4, x / y at +8 / +0xC in UI layout coordinates).
    ('cUI_Manager_receiveEvent', 0x00757840, 'func'),
    ('cEventMouseDown_vtable', 0x0089704C, 'data'),
    ('cEventMouseUp_vtable', 0x00899248, 'data'),
    # Inventories (src/gear/items/inventory.cpp): the manager of the 32 containers (backpacks, the chest ...) and the calls
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
    # Item tooltips (src/gear/items/itemtip.cpp): the reserved popup of the UI manager and the calls the inventory's hover uses.
    ('cUI_Manager_getPopup', 0x0075B1E0, 'func'),
    ('cUI_Popup_addFlags', 0x006E8860, 'func'),
    # SacredBild detours the start of this one and its own signature window covers the first 0x1B bytes, so the
    # signature is taken further in (0x6E6ED6, `mov al, [esp + 0x1C]`): the function is that address minus 0x36.
    ('cUI_Popup_setTextBody', 0x006E6ED6, 'code'),
    ('cUI_Popup_show', 0x00732550, 'func'),
    ('cUI_Control_clearFlags', 0x007325C0, 'func'),
    ('cUI_Popup_setOwner', 0x006E81C0, 'func'),
    # Item values (src/gear/items/itemtip.cpp): the item description's bonus line (stdcall (wchar_t** cursor, record, slot, context), ret 0x10).
    ('cItem_bonusLine', 0x005C4450, 'func'),
    # Item comparison (src/gear/items/itemtip.cpp): cCreature::equipSlot (thiscall (creature, item type) -> slot 0..0x12, 0x13 none, ret 4): the
    # equipment slot an item type goes to for this creature (the first free one of a group of rings, amulets or hands).
    ('cCreature_equipSlot', 0x0054ACC0, 'func'),
    ('engine_alloc', 0x008321A0, 'func'),
    # The C++ operator new (cdecl (bytes), 0x849842): what the game's constructors allocate their members with, freed by its operator delete.
    # (engine_alloc is another allocator: a small-block pool.)
    ('engine_new', 0x00849842, 'func'),
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
    # Saving (src/gear/save.cpp). The savegame window's event (code 0xF, sub 2; its class is made by the factory 0x8080A0
    # as `*p = &vtable`, 0x24 bytes) goes to cEngine::receiveEvent (0x618090, vtable slot 0 of the engine): it records
    # the slot and sets bit 0x40 of the engine's flags (+0x54), which the render thread loop turns into the write of
    # Save\GAMEnn.PAK. The engine is called through its vtable: SacredBild hooks the function's first bytes.
    ('cSaveEvent_vtable', 0x00890B38, 'data'),
    # The first free savegame number (cdecl () -> uint16, 0x619B80): 1.. while Save\GAME%02d.PAK exists, up to 0x1FD; a
    # save asked for with slot 0xFFFF takes it (event 0xF sub 2). Gear replaces it (the limit of 509 numbers).
    ('cSave_firstFreeSlot', 0x00619B80, 'func'),
    # Replaced menus (src/gear/ui/replace.cpp, src/gear/savegames.cpp): the savegame window's commands (cUI_Savegame, 0x71CC90,
    # thiscall (window, command), ret 4; what its buttons and message box answers run): 8 deletes the selected
    # savegame and refreshes the list, 10 refreshes the list, 0xC is its Back (closes it; in the menus the start
    # menu comes back).
    ('cUI_Savegame_command', 0x0071CC90, 'func'),
    # cEngine::captureInternal (thiscall (engine, bool capture), ret 4): reads the back buffer into the engine's 128 x 96
    # savegame picture (engine +0x1A4; 0 frees it). Only the engine's quicksave / autosave events and the savegame
    # request's thumbnail flag call it, so Save::request takes the picture itself between the world and the UI.
    ('cEngine_captureInternal', 0x00613670, 'func'),
    # The ability tables (src/gear/tables.cpp): T1 (ids from 1000, 0x78-byte records) and T2 (ids from 1, 0x94-byte
    # records) are static arrays that 0x4D1AF0 / 0x4D2310 scan by id; the lookups load the first record and compare
    # against the end (0x952A38 and 0x955344 are the end of T1 and of T2). The text lookup (the text table's instance
    # and `get(id)`) that the tables' name ids go through.
    ('abilityTable1First', 0x0094FE28, 'data', 0x004D1AFA),
    ('abilityTable1End', 0x00952A38, 'data', 0x004D1B08),
    ('abilityTable2First', 0x00952ACC, 'data', 0x004D231A),
    ('abilityTable2End', 0x00955344, 'data', 0x004D232A),
    # The region kill table (src/gear/tables.cpp "regions"): 32 records of 0x40 bytes at 0x00AAAD28 (record 0 unused), +0x30
    # the kills a region needs (initRegionKill, 0x42534F), +0x34 the boss flag, +0x38 the kills left, +0x3C the
    # region id. 0x425430 (the lookup by region id) starts with `mov ecx, 00AAAD28`.
    ('regionTable', 0x00AAAD28, 'data', 0x00425437),
    ('textResources_instance', 0x006725E0, 'func'),
    ('textResources_get', 0x00672C90, 'func'),
    # Realms (src/gear/realm.cpp). The network object (g_pNetPlayers, 0x7D7A70) holds the lobby fields: +0x38 an IP override,
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
    # Drops (src/gear/items/drops.cpp): the CRT rand and the range roll (rand % (hi - lo + 1) + lo, cdecl (lo, hi)) the loot code calls, the equipment
    # type picker (thiscall (type manager, category, level, 0, 0xF, hero preference, flags, 1, quality), ret 0x20) and the rarity table (a vector
    # of 8 byte entries; its begin pointer is at 00AD4438, the end at 00AD443C).
    ('crtRand', 0x00849B40, 'func'),
    ('rangeRand', 0x00443B20, 'func'),
    ('cItem_pickType', 0x005C7FA0, 'func'),
    ('g_rarityTable', 0x00AD4438, 'data', 0x0055A106),
    # Party (src/gear/world/party.cpp): companion heroes. A hero's stats block (creature +0x3A8) gains experience through
    # addExperience (thiscall (stats, amount), ret 4: levels up with the skill points, as a kill does); the engine's
    # own hero creation (0x60D9F0) finishes a new hero with initSpells (thiscall (stats, 0, -1), ret 8). isEnemy
    # (thiscall (creature, target) -> bool, ret 4) is the cursor's attack test; portraitImage (stdcall (type) ->
    # UI image index, ret 4) is the portrait the party window draws for a creature type. Orders reach a creature as
    # 0x44-byte events of this class through its vtable +0x18. The anti-cheat shadows (0x590730 checks them against
    # hero()) are two pages of XOR-keyed copies of experience, gold, level and the stats block.
    ('cStats_addExperience', 0x0057E160, 'func'),
    # setLevel (thiscall (stats, level, noWorldLevel, noSectorCap), ret 0xC): sets the level of a creature's stats block (creature
    # +0x3A8; level +0x3FE, the experience that level starts at) and recalculates the derived values, health included. The engine's
    # sector fill calls it (0x47C610) with (level, 1, 0); noSectorCap 1 takes the level as it is.
    ('cStats_setLevel', 0x00564D60, 'func'),
    # The level an amount of experience gives (thiscall (stats, experience) -> level, ret 4; "getLevelOnExp"): Gear finds
    # the experience a level starts at by searching it.
    ('cStats_levelOnExp', 0x0057F8D0, 'func'),
    # The weapon slots (keys 1-5, the taskbar's 0x6E1690): a hero's container (the inventory manager's, as its backpack)
    # keeps them at +0xC18 + slot * 0x18 (+0 main hand, +0xC off hand: item ids), the active one at +0xD58. Switching
    # sends the hero an order 0x114, takes the old weapons off (unequip, thiscall (creature, equipment slot 12 / 13, 1),
    # ret 8), makes the slot active (thiscall (container, slot, 0, 0, 1), ret 0x10) and puts the new ones on (equip,
    # thiscall (creature, equipment slot, item, 1), ret 0xC); a two-weapon hero (+0x1D8) takes the off-hand weapon in
    # slot 12 too and, unless +0x1D4, out of the container (thiscall (container, slot, 1, 0, 1), ret 0x10) into the
    # world at its feet. A changed weapon kind (0x546F80, thiscall (creature, 0) -> short) refreshes the motions
    # (0x54E6D0, thiscall (creature, 1, 0), ret 8). Container +0x1162 bit 0: locked (nothing is equipped then).
    ('cCreature_unequip', 0x00556A30, 'func'),
    ('cCreature_equip', 0x00555E00, 'func'),
    ('cContainer_selectWeaponSlot', 0x005D9860, 'func'),
    ('cContainer_takeWeapon', 0x005D8730, 'func'),
    ('cCreature_weaponKind', 0x00546F80, 'func'),
    ('cCreature_refreshMotions', 0x0054E6D0, 'func'),
    # A stats block works out the combat values anew (thiscall (stats, 0, 1), ret 8): zeroes the resistances, damage
    # bonuses and factors (floats at +0x66 .. +0xF2) and adds up level, items and buffs. Gear buffs add theirs after it.
    ('cStats_recalc', 0x005796A0, 'func'),
    # A text resource's new text (stdcall (id, wchar begin, end, capacity end), ret 0x10; takes the buffer, which it frees
    # with the engine's allocator): the network lobby names the heroes so (0x7EDC50, ids 0x2700 + slot - 1).
    ('textResources_set', 0x00672A60, 'func'),
    ('cStats_initSpells', 0x0055FC00, 'func'),
    # A hero's combo slot (thiscall (stats, index, u32 arts[4]*, u16 levels[4]*, combo id), ret 0x10): what the combo
    # master writes; the hero creation clears the four slots with it (ids 0x200..0x203, 0x55E30F).
    ('cStats_setCombo', 0x005813D0, 'func'),
    # Potions as the potion keys use them (usePotion 0x612FA0): a container's potion of a kind (thiscall (container,
    # kind, by missing health), ret 8 -> item id; kind 1 healing, 2 mana, 4 ...) and a creature drinking an item it
    # carries (thiscall (creature, item id) -> bool, ret 4); the caller then takes the item out of the container.
    ('cInventory_findPotion', 0x005DA8C0, 'func'),
    ('cCreature_useItem', 0x0054AA80, 'func'),
    ('cCreature_isEnemy', 0x00548F60, 'func'),
    ('creaturePortraitImage', 0x00435D30, 'func'),
    ('g_creatureEventVtable', 0x0089095C, 'data'),
    ('g_antiCheatShadow1', 0x0182EE5C, 'data'),
    ('g_antiCheatShadow2', 0x0182EE60, 'data'),
    # Custom classes (src/gear/world/classes.cpp). classOf (fastcall (creature) -> hero class 1..9) picks the action handler of a hero's
    # combat arts; the motion player (thiscall (creature, slot, three floats), ret 0x10) starts a motion slot; the learn function
    # (thiscall (stats, 0x16-byte entry by value), ret 0x18) appends an ability; the model manager (global pointer) holds the
    # Models.tmp records.
    ('cCreature_classOf', 0x005500D0, 'func'),
    ('cCreature_playMotion', 0x00593320, 'func'),
    ('cStats_learnAbility', 0x00580750, 'func'),
    ('g_pModelManager', 0x00AA4538, 'data'),
    # The world under the mouse (src/gear/world/classes.cpp: gear.world.cursor): the world view's pick (thiscall (view, x, y, exclude id, rect*) ->
    # object id under physical pixel (x, y), ret 0x10); the view comes from the engine (g_uiEventTarget: +8 the views, +0x48 the index)
    # and its vtable +0x18 turns pixels into a world point.
    # SacredBild detours the pick's first bytes (aim assist): found by a window 0x30 bytes inside, called from the entry (through its detour).
    ('cWorldView_pick', 0x00626C80, 'code'),
    # Ability lookups by id (cdecl (short id) -> record*, T1 0x78-byte records / T2 0x94-byte records; the record's first word is
    # the id the class handlers switch on) and the class checks a rune read makes (thiscall (record, hero type, flag) -> bool,
    # ret 8).
    ('abilityLookup1', 0x004D1AF0, 'func'),
    ('abilityLookup2', 0x004D2310, 'func'),
    ('abilityUsable1', 0x004D1B20, 'func'),
    ('abilityUsable2', 0x004D24C0, 'func'),
    # Arts whose numbers live in a spell record (thiscall (art record) -> spell id, 0 = none): 0x4D1E10 maps 0x3FA, 0x3FB, 0x414, 0x419..0x41D.
    ('abilityLinkedSpell', 0x004D1E10, 'func'),
    # Potions (src/gear/items/itemuse.cpp): cPotion3D::receiveEvent (thiscall (item, message*), ret 4) runs the effect of a drunk potion; the
    # event `item.use` asks the mods first. The teleporter list is opened by a message to the kernel's bus, as the script command
    # Teleporter does (0x4AE040): the kernel (0x808E50 returns it), its send (thiscall (message, 0, 0), ret 0xC).
    ('cPotion3D_receiveEvent', 0x005CAA60, 'func'),
    ('kernel_instance', 0x00808E50, 'func'),
    ('kernel_send', 0x008092F0, 'func'),
    # cCreature::receiveEvent (vtable slot 6, thiscall (creature, message*), ret 4): every effect of a drunk potion reaches the drinker
    # as a message of kind 6 (word +5 the effect, +6 its value); while a potion's effect is suppressed (`item.use` answered 'consume'
    # or 'keep') the hook drops these. 0x182EE48 is the game kind flag the script command Teleporter and ReBorn's teleporter use to
    # choose the sub-command (6 when it is 1, else 5).
    ('cCreature_receiveEvent', 0x0052E590, 'func'),
    ('g_portalListFlag', 0x0182EE48, 'data'),
    # The combo master's rune exchange tab (cUI_MasterLotto, src/gear/items/master.cpp; docs/GearApi.md "master.price"). Render (thiscall (tab, device), 006D49A0,
    # the vtable's slot 5), layout of the controls (fastcall (tab), 006D5B80), the exchange itself (fastcall (tab), 006D4520: takes the runes of the slots
    # and makes the chosen rune, the event handler pays before it). The gold the exchange costs is a table by the number of runes in the slots (dword[5] at
    # 00AD4B2C, filled at run time): found through `mov eax, [edx*4 + 00AD4B2C]` in the price getter 006D3F30.
    ('masterLayoutFn', 0x006D5B80, 'func'),
    ('masterExchangeFn', 0x006D4520, 'func'),
    ('masterPriceTable', 0x00AD4B2C, 'data', 0x006D3F51),
    # The hero's ability list (stats +0x102): the index of an ability id (thiscall (stats, id) -> index, the list's size when it has none, ret 4) and
    # the engine's level raise (thiscall (stats, index, amount, byLevel, 0) -> bool, ret 0x10: adds to the level byte (+6) or the item bonus (+7), marks it
    # changed). 00581F80 is what the hero's skill purchase uses. ENG 0043AF10: the ability a rune item type teaches (thiscall on the type manager (item type)).
    ('cStats_abilityIndex', 0x00581D40, 'func'),
    ('cStats_raiseAbility', 0x00581F80, 'func'),
    ('typeAbilityOfRune', 0x0043AF10, 'func'),
    # The text of an art / a spell at a level (thiscall (record, wstring* out, level, format), ret 0xC) and a control's screen rect (thiscall (control,
    # int[3]), ret 4): the combo master page draws the chosen art's "<name> - Level <n>" and its maximum level message with them (src/gear/items/master.cpp).
    ('abilityDescribe1', 0x004D2020, 'func'),
    ('abilityDescribe2', 0x004D27B0, 'func'),
    ('cUI_Control_getRect', 0x00732350, 'func'),
    # The kill statistics record (src/gear/world/killstat.cpp): thiscall on the statistics block, 17 stack words (ret 0x44); the death handlers call it for
    # a kill of the local hero or of a creature it owns. The event `kill.recorded` follows it.
    ('cKillStat_record', 0x00424250, 'func'),
    # The potion picker (src/gear/items/potionpick.cpp): stdcall (mode, flag, tier), ret 0xC; merchants and the loot of dying creatures call it. The event
    # `potion.pick` asks the mods first.
    ('potionPick', 0x0043B1A0, 'func'),
    # The game's calendar singleton (src/gear/world/world.cpp clockMinutes): cdecl () -> object, +0x20 hour, +0x22 minute.
    ('gameClock', 0x00417560, 'func'),
    # The merchant window's stock (src/gear/items/shop.cpp; docs/GearApi.md "gear.shop", event `shop.stock`). The window's event handler (thiscall (window, event),
    # ret 4; event kind 5 / id 0x2E with the sub-kind in +8: 8 a merchant opened, 9 the refresh message) rebuilds or tops up the stock: the clear
    # (thiscall (window, all), ret 4: all = 1 empties the grid, 0 removes a random handful), the random stock (thiscall (window, count), ret 4) and the
    # one-item stocker (thiscall (window, item type, quality, 0), ret 0xC: finds room in the grid at window +0x154, makes the item and rolls its
    # properties with the item generator, cdecl with 9 arguments: level, quality, ..., rarity 0..3 in the fourth).
    ('merchantReceive', 0x006DAD90, 'func'),
    ('merchantClear', 0x006D9B90, 'func'),
    ('merchantRestock', 0x006DA320, 'func'),
    ('merchantAddItem', 0x006D9DE0, 'func'),
    ('itemGenerate', 0x005D5080, 'func'),
    # Skill particle effects (src/gear/world/pfx.cpp). cObjectFX's effect setup (thiscall (fx, effect id), ret 4: 0x5A0C50 picks the hard-coded
    # particle system class for the id 0x300..0x49D and registers it), the particle manager's add (thiscall (manager, system) -> handle,
    # ret 4) and its update by handle (thiscall (manager, handle, dt), ret 8), the manager singleton (+0x18 the vector of systems by
    # handle), the texture lookup by name (thiscall (texture manager, name) -> handle, ret 4), the shared sprite drawing of the particle
    # systems (thiscall (system, device, ...), ret 0x60).
    ('cObjectFX_setup', 0x005A0C50, 'func'),
    ('particleMgr_add', 0x007CB830, 'func'),
    ('particleMgr_update', 0x007CC080, 'func'),
    ('g_pParticleMgr', 0x017EEBCC, 'data'),
    ('textureMgr_find', 0x00657360, 'func'),
    ('particle_drawSprites', 0x00761B00, 'func'),
    ('particle_drawGroundSprite', 0x007631E0, 'func'),
]
# The functions that hold site hooks (tools/sigs/gear_sites.py): found by signature, the site is at a fixed offset in them.
GEAR += [(name, va, 'func') for name, va in gear_sites.functions().items()]


def main():
    ref = sigs.Image(gen_sigs.SACRED_ENG, _A)
    others = gen_sigs.other_builds(gen_sigs.SACRED_DE)
    path = paths.GEAR_SIGS
    _, failed = gen_sigs.emit(path, '', 'Gear::Addr', GEAR, ref, others, 'kGearSigs')
    with open(path, newline='\n') as f:
        text = f.read().replace('tools/sigs/gen_sigs.py', 'tools/sigs/gen_gear_sigs.py', 1)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    # src/gear/engine/gear_sites.inc: the site table the C++ side walks
    lines = ['// Generated by tools/sigs/gen_gear_sigs.py from tools/sigs/gear_sites.py -- do not edit by hand.',
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
    dl = ['// Generated by tools/sigs/gen_gear_sigs.py from tools/sigs/gear_sites.py -- do not edit by hand.']
    dl.append('static const uint32_t kLootRandCalls[] = {' + ', '.join(f'0x{a:X}' for a in gear_sites.LOOT_RAND_CALLS) + '};')
    dl.append('static const RarityCallSpec kRarityCalls[] = {')
    for kind, rname, rfn, rsite in gear_sites.RARITY_CALLS:
        dl.append(f'    {{{0 if kind == "range" else 1}, &Gear::Addr::{rname}, 0x{rsite - rfn:X}}}, // {rsite:08X} in {rfn:08X} {kind}')
    dl.append('};')
    dl.append(f'static const uint32_t kLootSize = {gear_sites.LOOT_FUNCTION[2]};')
    fnname, fn = gear_sites.SLOT_FUNCTION
    lines.append(f'static const uint32_t kKeySite = 0x{gear_sites.KEY_SITE - fn:X}, kKeyEnd = 0x{gear_sites.KEY_END - fn:X};')
    lines.append('static const uint32_t kKeyFull[5] = {' + ', '.join(f'0x{a - fn:X}' for a in gear_sites.KEY_FULL) + '};')
    lines.append('static const uint32_t kKeySelect[5] = {' + ', '.join(f'0x{a - fn:X}' for a in gear_sites.KEY_SELECT) + '};')
    with open(paths.GEAR_SITES, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    with open(paths.GEAR_DROPS, 'w', newline='\n') as f:
        f.write('\n'.join(dl) + '\n')
    if failed:
        sys.exit(f'{len(failed)} entries without a signature')


if __name__ == '__main__':
    main()
