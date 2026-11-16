#pragma once
#include "sacred/address.h"

#include <cstdint>

// The sacred.exe addresses Sacred Gear uses (English GOG build in the comments). Found at startup by signature
// (gear_sigs.inc, written by tools/gen_gear_sigs.py), never fixed: an address whose signature does not match stays 0
// and the feature that needs it stays off. tools/check_gear_overlap.py checks that none of them shares bytes with
// SacredBild's, so the two work in either load order.
namespace Gear::Addr
{
    // ENG 0060DD20. The in-game loop (cEngine_renderThreadRun, 0x60E350) calls it once per frame after the world
    // update and before the world view renders, outside loading screens, with the frame time in seconds.
    inline Sacred::Thiscall<void(void* engine, float dt)> cEngine_updateZoom{};

    // ENG 00AAB5E4. The object that holds the creature types loaded from Pak\Creature.pak (tables.cpp); nullptr
    // before the game created it.
    inline Sacred::Global<void*> g_pTypeManager{};

    // Objects. ENG 00AD5C40: the object manager, nullptr before a game ran; its +4 / +8 are the begin and end of a
    // vector of object pointers indexed by id (ids 1..16 are the players' heroes).
    inline Sacred::Global<void*> g_pObjectManager{};
    // ENG 00603E30: the local player's hero, nullptr if there is none.
    inline Sacred::Thiscall<void*(void* manager)> cObjectManager_hero{};
    // ENG 005FBA40 ("cObjectManager::create"): creates an object of type `type` (an index into the 0x80-byte type
    // records) at `pos` and returns its id, 0 if it failed. The engine calls it with (type, pos, 0, 1, 0) for the
    // hero (0060D9F0) and (type, pos, 0, 0, x) for the script command CreateNPC; `broadcast` sends the creation to
    // the network peers in a network game.
    inline Sacred::Thiscall<uint32_t(void* manager, uint32_t type, void* pos, int a3, int broadcast, int a5)>
        cObjectManager_create{};
    // ENG 005FCCB0 ("cObjectManager::pop"): places object `id` at `pos` (teleports it); false if it could not.
    inline Sacred::Thiscall<uint32_t(void* manager, uint32_t id, void* pos)> cObjectManager_pop{};
    // ENG 0084A961: MSVC's __RTDynamicCast; with the two type descriptors (ENG 008EB648 cObject, 008EB660
    // cCreature) it tells creatures from other objects.
    inline Sacred::Cdecl<void*(void* object, long vfDelta, void* source, void* target, int isReference)>
        rtDynamicCast{};
    inline Sacred::Global<uint8_t> cObject_typeDescriptor{};
    inline Sacred::Global<uint8_t> cCreature_typeDescriptor{};

    // Automation (gear/automation.cpp). ENG 00CDCA1C: the application object; +0xB0 is the game window, which the
    // menu's "load savegame" posts its message to (007561E0).
    inline Sacred::Global<uint8_t*> g_pApp{};
    // ENG 017EEBBC: the UI manager (SacredBild's src/sacred/ui.h has its layout), nullptr before the menus exist.
    inline Sacred::Global<uint8_t*> g_pUiManager{};
    // ENG 0182EBE8: the players; +0x14 is the local hero's id, which loading a savegame sets to 1 first.
    inline Sacred::Global<uint8_t*> g_pNetPlayers{};
    // ENG 00654F60: the mouse (made on first use); +4 / +8 the cursor in UI layout coordinates.
    inline Sacred::Cdecl<uint8_t*()> cMouse_instance{};

    // Mod drawing (gear/ui.cpp), inside cUI_Manager_render (ENG 00758ED0). ENG 00758DC0: the popup pass,
    // thiscall (manager, device, flag), after the windows and before the escape menu and the message box.
    inline Sacred::Thiscall<void(void* manager, void* device, int flag)> cUI_Manager_renderPopups{};
    // ENG 0075934F: `call 006D01F0` (the party arrows, cdecl (device)), the in-game branch's first draw, before any
    // window. ENG 00759585: `call 00649BE0` (the font manager), after the message box in every mode; EBX is the device.
    inline uintptr_t uiRenderWorldCall{};
    inline uintptr_t uiRenderTopCall{};
    // ENG 00CDCA70: the UI fonts, a vector of cFont* by id at +0 / +4 (made by 00649BE0).
    inline Sacred::Global<uint8_t*> g_pFontManager{};
    // ENG 013E7838 / 0065ED90: the texture manager and its get(handle, 0) -> cTexture* (+0x14 the surface): stamps
    // the texture as used and loads it if it isn't.
    inline Sacred::Global<void*> g_pTextureManager{};
    inline Sacred::Thiscall<uint8_t*(void* manager, uint32_t handle, int flags)> cTextureManager_get{};
    // ENG 009ECED8: +0x28 of the first UI image record (0x54 bytes from 009ECEB0; the last is named "INVALID_MAX").
    inline Sacred::Global<uint32_t> uiImageTextures{};

    // Items and pickup (gear/world.cpp, gear/pickup.cpp). ENG 0094EBC0: cItemBase's RTTI type descriptor.
    inline Sacred::Global<uint8_t> cItemBase_typeDescriptor{};
    // ENG 00558080 ("pickupItem"): moves world item `id` to the creature (gold to its purse); 1 if it did. The click
    // pickup (0053CEB0), the collect behaviour (004ECF69, 004ED095) and the host's message 0x20C call it, none looks
    // at the result. Returning 0 before it runs leaves everything as it was.
    inline Sacred::Thiscall<uint32_t(void* creature, uint32_t id)> cCreature_pickupItem{};
    // ENG 004EC330: whether the collect behaviour (004EC560) takes `item` (settings.cfg PICKUPAUTO).
    inline Sacred::Thiscall<uint8_t(void* creature, void* item)> cCreature_collectFilter{};
    // ENG 005C13C0: the item's label colour (ARGB) from its data (`record`, nullptr: the item's own at +0x118).
    inline Sacred::Thiscall<uint32_t(void* item, void* record)> cItem_labelColor{};

    // Net trace (gear/netmsg.cpp). ENG 007D4860: the client's send queue, thiscall (net, message*, flush), ret 8. A
    // message is a 16-byte header (u32 size | slot << 22 in the low 22 / high bits, +0xE u16 message id) and its payload.
    inline Sacred::Thiscall<uint32_t(void* net, uint32_t* message, uint8_t flush)> cNet_queueMessage{};
    // The mod channel (gear/netmsg.cpp). ENG 007D4300: build a message (16-byte header, the sender slot from net +0x48)
    // and queue it, thiscall (net, data, size, type, flush), ret 0x10. ENG 0182EBF0: the connection object (may be null;
    // +0x2A and +0x2B must be set to send). ENG 007DCF91: in the receive dispatcher 0x7DC260 (engine thread), the type
    // switch `cmp ecx, 0x1D4` / `ja <next message>` / `xor edx, edx` / `mov dl, [ecx + index table]` /
    // `jmp [edx*4 + jump table]`; at the jump: [esp+0x2C] the message, ebp its payload, ebx the payload size, esi the
    // sender slot (0: the server).
    inline Sacred::Thiscall<uint8_t(void* net, const void* data, uint32_t size, uint32_t type, uint32_t flush)> cNet_sendMessage{};
    // Gold (gear/world.cpp). ENG 00580110: the hero stats' gold setter, thiscall (stats = creature + 0x3A8, gold), ret 4:
    // the anti-cheat check (0x590730), addGold (0x57FD80: clamps to 0 .. 0x7FFFFFFE, sets the values-dirty flag
    // creature +0x4CF, so the next update sends message 0x11B, refreshes the checked copy), then a new snapshot.
    // ENG 00461B74: a `call 00580110` (after `lea ecx, [ebp + 0x3A8]`) the setter is read from (gear/world.cpp).
    inline uintptr_t cStats_setGoldCall{};
    inline Sacred::Global<uint8_t*> g_pNet{};
    inline uintptr_t netDispatchSwitch{};

    // Mod UI input (gear/ui.cpp). ENG 00757840: cUI_Manager::receiveEvent (vtable slot 0); cEngine_receiveEvent
    // (00618090) gives it every mouse and key event first, and the world gets a mouse event only if it returns
    // false. ENG 0089704C / 00899248: the vtables of the mouse button down / up events (+4 button, +8 / +0xC x, y
    // in UI layout coordinates).
    inline Sacred::Thiscall<uint8_t(void* manager, void* event)> cUI_Manager_receiveEvent{};
    inline Sacred::Global<uint8_t> cEventMouseDown_vtable{};
    inline Sacred::Global<uint8_t> cEventMouseUp_vtable{};

    // Inventories (gear/inventory.cpp). ENG 00AD4448: the inventory manager (made by 005DEBC0), nullptr before a
    // game ran: 32 containers of 0x1180 bytes. A container (the backpack of the hero with object id n is container
    // n, the chest is container 0x11): +0x18 + cell * 12 the cells, a grid of +0x115C columns by +0x115E rows with
    // cell = row * columns + column (dword item id on an item's top-left cell, 0 on the others; +4 flags, bit 0 =
    // taken; on the item's cell +5 / +6 its width and height, on its other cells the column / row of its top-left
    // cell), +0x1160 the container's number.
    inline Sacred::Global<void*> g_pInventory{};
    // ENG 005DABA0: finds room for a w x h item; stores the cell in *slot, true if there is room. `byRow`: scan row
    // by row instead of column by column. ret 0x10.
    inline Sacred::Thiscall<uint8_t(void* container, uint32_t w, uint32_t h, uint16_t* slot, int byRow)>
        cInventory_findSpace{};
    // ENG 005D9F00: puts item `id` (w x h) at `slot`: sets the cells, takes the item out of the world, tells the
    // item its owner. 0 if the cells are not free. `notify`: tell the network peers (the engine passes 1 for a
    // click, 0 when it sorts). ret 0x14.
    inline Sacred::Thiscall<uint32_t(void* container, uint32_t slot, uint32_t id, uint32_t w, uint32_t h, int notify)>
        cInventory_put{};
    // ENG 005DA630: takes item `id` out of this container (the cells are cleared; the item object stays); 1 if it
    // was in it. ret 8.
    inline Sacred::Thiscall<uint32_t(void* container, uint32_t id, int notify)> cInventory_remove{};
    // Item icons (gear/itemicon.cpp), ENG 00640640: thiscall (item), loads the item's model resources (the inventory
    // does it for every item it draws that is not loaded yet); 00401660: cdecl (), the model renderer's singleton;
    // 0040E9C0: thiscall (renderer, device, model0, model1, x, y, type, scale, 0, flags, 0, 0), ret 0x2C: draws the
    // item model of `type` centered at (x, y) of the 1024x768 layout, `scale` 1 for the inventory's size, `model0/1` the
    // two dwords at item +0x40 (its Granny model instance); 00643110 / 00643430: the render flags' instance and set
    // (thiscall (flag, on)), see docs/RE_NOTES.md.
    inline Sacred::Thiscall<void(void* item)> cItem_loadResources{};
    inline Sacred::Cdecl<void*()> modelRenderer_instance{};
    inline Sacred::Thiscall<void(void* renderer, void* device, uint32_t model0, uint32_t model1, int x, int y,
                                 uint32_t type, float scale, int a8, uint32_t flags, int a10, int a11)>
        modelRenderer_drawItem{};
    inline Sacred::Cdecl<void*()> renderFlags_instance{};
    inline Sacred::Thiscall<uint32_t(void* flags, uint32_t flag, uint32_t on)> renderFlags_set{};
    // ENG 00428D60 (stdcall (type, uint16* w, uint16* h), no `this`): the size in cells of an item type.
    inline Sacred::Thiscall<uint32_t(void* unused, uint32_t type, uint16_t* w, uint16_t* h)> cItemType_size{};
    // ENG 005FBDB0 ("cObjectManager::destroy"): destroys object `id`; the engine calls it with (id, 1, 0, 0) after
    // taking an item out of its inventory (use of a potion, 005BFF60). ret 0x10.
    inline Sacred::Thiscall<void(void* manager, uint32_t id, int a2, int a3, int a4)> cObjectManager_destroy{};
    // ENG 0042BF20 (stdcall (record)): the number an item keeps at +0x120, made from its 0x82-byte data at +0x118
    // (the savegame loader sets it so after reading an item, 005C7690).
    inline Sacred::Thiscall<uint32_t(void* unused, const void* record)> cItem_dataHash{};
    // ENG 005C9230: inserts `count` copies of the 0x82 bytes at `fill` at `pos` into the item's vector of 0x82-byte
    // entries (begin / end / capacity pointers at +0x100 / +0x104 / +0x108 of the item; `this` is item + 0x100).
    // ret 0xC.
    inline Sacred::Thiscall<void(void* vector, void* pos, uint32_t count, const void* fill)> cItem_entriesInsert{};
    // ENG 005BFF60 ("insert upgrade"): thiscall (item, upgrade item id, socket) -> 1 if it went in, ret 8. The
    // blacksmith's socket window calls it (006A2660): the upgrade item (a rune) is checked (cItemBase, free socket
    // left: the item's record +0x124..+0x12B counts the sockets), its bonuses are added to the item's record (+0x12C
    // ...), its own 0x82-byte record is appended to the item's entries and the upgrade item is destroyed.
    inline Sacred::Thiscall<uint8_t(void* item, uint32_t upgradeId, uint32_t socket)> cItem_insertUpgrade{};
    // ENG 005C0810: thiscall (item, record*, notify) ret 8: copies the 0x82-byte record into the item (+0x118), makes
    // the number at its +8 and, in a network game with notify, announces the whole item (its entries and record, as
    // the engine does after socketing or repairing: message 0x16C) to the peers.
    inline Sacred::Thiscall<void(void* item, void* record, uint32_t notify)> cItem_setRecord{};
    // ENG 00451300 ("cObjectFactory::create", stdcall (kind, type, id) -> object, ret 0xC): allocates and constructs an object
    // of the class `kind` (the type record's +0x2E) and returns it WITHOUT registering it with the object manager (that is
    // 005FB530's next step): no id (+0x0C stays 0), no sector list, nothing the network sees. Items are 0x1E4 bytes. The
    // object's own virtual destructor (slot 0, flag 1: delete and free) takes it down again.
    inline Sacred::Thiscall<void*(void* unused, uint32_t kind, uint32_t type, uint32_t id)> cObjectFactory_create{};
    // The model loader behind an item's model state (cItemBase +0x40 / +0x44, made by 0x0044C330): `0x004115D0` is its
    // singleton (cdecl), `0x00412590` (thiscall (loader, name, out[2], description) ret 0xC, true if loaded) fills the two
    // dwords the model renderer takes, `0x00426250` / `0x004262B0` (thiscall (type manager, type) ret 4) are the model name
    // and description of a type, `0x00403E10` (thiscall (renderer, model0, model1) ret 8) gives a pair back. Gear draws an
    // item's icon from these alone, with no item object at all.
    inline Sacred::Cdecl<void*()> modelLoader_instance{};
    inline Sacred::Thiscall<uint8_t(void* loader, const char* name, uint32_t* out, void* description)> modelLoader_load{};
    inline Sacred::Thiscall<const char*(void* typeManager, uint32_t type)> cTypeManager_modelName{};
    inline Sacred::Thiscall<void*(void* typeManager, uint32_t type)> cTypeManager_modelDescription{};
    inline Sacred::Thiscall<void(void* renderer, uint32_t model0, uint32_t model1)> modelRenderer_free{};
    // ENG 005CF2D0: cWeapon3D's network read / write (vtable slot 8, thiscall (stream, write, code), ret 0xC). Gear
    // only compares an item's slot 8 with it to tell weapons, which keep four more dwords (+0x19C, +0x1AC, +0x1A0,
    // +0x1BC) that the engine's save and network code copy after the common item data.
    inline Sacred::Thiscall<uint32_t(void* weapon, void* stream, int write, uint32_t code)> cWeapon3D_netIO{};

    // Saving (gear/save.cpp). ENG 00890B38: the vtable of the savegame window's event (0x24 bytes; slot 1 returns the
    // code, 0xF). The event goes to cEngine::receiveEvent (ENG 00618090, vtable slot 0 of the engine object, thiscall
    // (engine, event) -> 1, ret 4; SacredBild hooks its first bytes, so it is called through the vtable): it records
    // the slot (uint16 at engine+0x68) and sets bit 0x40 of the dword at engine+0x54 (already set: returns at once,
    // busy); the render thread loop (0060E350) then writes "SAVE\GAME%02d.PAK" (00619BF0) and clears bits 0xC0.
    inline Sacred::Global<uint8_t> cSaveEvent_vtable{};
    // The ability tables (gear/tables.cpp), static arrays in .data: T1 (ENG 0094FE28..00952A38, 0x78-byte records, ids from
    // 1000, scanned by 004D1AF0) and T2 (ENG 00952ACC..00955344, 0x94-byte records, ids from 1, scanned by 004D2310). Each
    // is the address of the first record / one past the last (found in the lookups' own code).
    // Site hooks (gear/sitehook.cpp, tools/gear_sites.py): the functions that hold a site, found by signature. The level to
    // factor expression of ENG 0051CC30 (the whole function) and its inlined copies in 0050E6C0 and 005893B0.
    inline uintptr_t levelFn51CC30{};
    inline uintptr_t levelFn50E6C0{};
    inline uintptr_t levelFn5893B0{};
    // The functions with constant sites (the hit handlers 005038F0, 00506770, 0050AC00 and 0050E6C0 (levelFn50E6C0), 00559BF0, 0056B130).
    // The creature AI step (ENG 004D4E50) with the sites where it picks the ability a creature uses.
    inline uintptr_t abilityAiFn{};
    // The effect-entry function (ENG 00562470): add a value to a creature's effect entries, clamp to their maximum.
    inline uintptr_t effectAddFn{};
    // The item bonus roller (ENG 00571710).
    inline uintptr_t bonusFn{};
    // The event handler of the skill slot window (ENG 006E1BC0), with the places where a digit key selected a slot.
    inline uintptr_t skillSlotFn{};
    // Quick cast. ENG 00899248: the vtable of the UI's mouse event class; 00AD6EF8: the object the UI events go to; 0182EE18 / 0182EE1C: the
    // cursor; 006172C0: the window event handler (thiscall (target, event, 0)).
    inline Sacred::Global<uint8_t> g_clickEventVtable{};
    inline Sacred::Global<uint8_t> g_clickDownEventVtable{};     // ENG 0089704C: the mouse button down event class
    inline Sacred::Global<uint8_t*> g_uiEventTarget{};
    inline Sacred::Global<int32_t> g_cursorX{};
    inline Sacred::Global<int32_t> g_cursorY{};
    inline uintptr_t cWindow_handleEventInner{};     // the handler is 0x30 bytes before this
    inline uintptr_t siteFn51D840{};         // the ability dispatch (ENG 0051D840)
    inline uintptr_t siteFnColor{};          // ENG 004DCB80
    inline uintptr_t siteFn5038F0{};
    inline uintptr_t siteFn506770{};
    inline uintptr_t siteFn50AC00{};
    inline uintptr_t siteFn559BF0{};
    inline uintptr_t siteFn56B130{};

    // Realms (gear/realm.cpp). The network object (g_pNetPlayers) keeps the lobby fields: +0x38 the IP override (0 =
    // none), +0x3C the port, +0x39E4 the host (a string: begin, end, capacity pointers). ENG 007D8560: what the
    // network menu runs when a network screen opens, thiscall (net, flag), ret 4; it calls 007EDEA0 (thiscall (net):
    // Settings.cfg into the object) and then makes the game client's network and the lobby connection, which read
    // the port with 0081E400 (thiscall (config) -> NETWORK_LOBBYPORT) at 007CCD28. ENG 0182EBD0: the lobby connection (nullptr until then).
    // ENG 00415BD0: the string's assign, thiscall (string*, first, last, allocator*), ret 0xC: the call the engine's
    // own settings reader copies NETWORK_LOBBY in with.
    inline Sacred::Global<uint8_t*> g_pLobby{};
    inline Sacred::Thiscall<void(void* net, uint32_t flag)> cNet_openScreen{};
    inline Sacred::Thiscall<void(void* net)> cNet_loadSettings{};
    // ENG 007EE780: the object's lobby fields back into Settings.cfg (thiscall (net)); the login and register calls and
    // 007D8560 run it.
    inline Sacred::Thiscall<void(void* net)> cNet_saveSettings{};
    inline uintptr_t lobbyPortCall{};     // ENG 007CCD28: `mov ecx, 0182EE78; call 0081E400` in the lobby connection's constructor
    inline Sacred::Thiscall<void*(void* string, const char* first, const char* last, void* allocator)> cString_assign{};

    // ENG 0182EE44: the difficulty level of the running game (0 .. 4).
    inline Sacred::Global<int32_t> g_difficulty{};
    inline Sacred::Global<uint8_t> abilityTable1First{};
    inline Sacred::Global<uint8_t> abilityTable1End{};
    inline Sacred::Global<uint8_t> abilityTable2First{};
    inline Sacred::Global<uint8_t> abilityTable2End{};
    // The game's text table (ENG 006725E0: the instance, cdecl; 00672C90: thiscall (instance, text id) -> a pointer to
    // the wide string's pointer; an id the table does not have gives an empty string). Text ids are the ones the data
    // tables hold (not the keys of global.res).
    inline Sacred::Cdecl<void*()> textResources_instance{};
    inline Sacred::Thiscall<const wchar_t* const*(void* self, uint32_t id)> textResources_get{};


    // Item tooltips (gear/itemtip.cpp): what the inventory windows do when the cursor rests on an item. The equipment
    // window's hover (006B7700) takes the UI manager's reserved popup (u16 at manager +0x54, ENG 0075B1E0 (index) ->
    // cUI_Popup*), builds the item's text with its virtual function 17 (thiscall (std::vector<wchar_t>* out, record or
    // 0, 0, 0x5F) ret 0x10: name in the rarity colour, price, protection, level and the bonus lines as colour codes),
    // sets the popup's flags (006E8860 (0xA): clamp to the screen), its x / y (+0x24 / +0x28: the popup is centered on
    // them in the 1024x768 layout), its text (006E6EA0 (0, vector*, 0) ret 0xC), shows it (00732550 (1)) and ties it
    // to the item (006E81C0 (item id)). The vector's buffer comes from the engine's allocator (008321A0 (bytes), cdecl)
    // and is freed with 0041D3B0 (stdcall (ptr, count)).
    inline Sacred::Thiscall<void*(void* manager, uint32_t index)> cUI_Manager_getPopup{};
    inline Sacred::Thiscall<void(void* popup, uint32_t flags)> cUI_Popup_addFlags{};
    // ENG 006E6EA0 (ret 0xC): found by its body (see gen_gear_sigs.py), the entry is 0x36 bytes before.
    inline uintptr_t cUI_Popup_setTextBody{};
    inline Sacred::Thiscall<void(void* popup, uint32_t a, void* text, uint8_t b)> cUI_Popup_setText(){ return {cUI_Popup_setTextBody - 0x36}; }
    inline Sacred::Thiscall<void(void* popup, uint32_t flag)> cUI_Popup_show{};
    // ENG 007325C0: the counterpart of 00732550 (any cUI_Control2: flags |= bits there, flags &= ~bits here; bit 1 is
    // "visible": the window's vtable +0x24 is told). ret 4. The automation helper that shows or hides a window uses both.
    inline Sacred::Thiscall<void(void* control, uint32_t bits)> cUI_Control_clearFlags{};
    inline Sacred::Thiscall<void(void* popup, uint32_t itemId)> cUI_Popup_setOwner{};
    inline Sacred::Cdecl<void*(uint32_t bytes)> engine_alloc{};
    inline Sacred::Thiscall<void(void* unused, void* ptr, uint32_t count)> engine_free{};

    // Drops (gear/drops.cpp). ENG 00849B40: the CRT rand (cdecl () -> 0 .. 0x7FFF, one LCG state per thread); ENG 00443B20: cdecl (lo, hi) ->
    // rand() % (hi - lo + 1) + lo. ENG 005C7FA0: the equipment type picker, thiscall (type manager at 00AAB5E4, category, level, 0, 0xF, hero
    // preference, flags, 1, quality) -> item type id, ret 0x20. ENG 00AD4438: the rarity table, a vector (begin at +0, end at +4) of 8 byte
    // entries {dword id, word weight, word pad}; the loot code walks it with a roll of 1 .. 10000.
    inline Sacred::Cdecl<int()> crtRand{};
    inline Sacred::Cdecl<int(int lo, int hi)> rangeRand{};
    inline Sacred::Thiscall<uint32_t(void* typeManager, uint32_t category, uint32_t level, uint32_t a3, uint32_t a4, uint32_t heroPref,
                                     uint32_t flags, uint32_t a7, uint32_t quality)> cItem_pickType{};
    inline Sacred::Global<uint8_t*> g_rarityTable{};
    // The functions that hold drop sites: the creature loot function (ENG 00559BF0, thiscall (creature, forced type, rarity bonus, survival
    // float, killer id), ret 0x10) and three more with a rarity draw (ENG 00485C10, 004AC940, 005D57C0).
    inline uintptr_t lootRollFn{};
    inline uintptr_t rarityFn485C10{};
    inline uintptr_t rarityFn4AC940{};
    inline uintptr_t rarityFn5D57C0{};
}
