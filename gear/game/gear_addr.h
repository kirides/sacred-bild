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

    // Mod UI input (gear/ui.cpp). ENG 00757840: cUI_Manager::receiveEvent (vtable slot 0); cEngine_receiveEvent
    // (00618090) gives it every mouse and key event first, and the world gets a mouse event only if it returns
    // false. ENG 0089704C / 00899248: the vtables of the mouse button down / up events (+4 button, +8 / +0xC x, y
    // in UI layout coordinates).
    inline Sacred::Thiscall<uint8_t(void* manager, void* event)> cUI_Manager_receiveEvent{};
    inline Sacred::Global<uint8_t> cEventMouseDown_vtable{};
    inline Sacred::Global<uint8_t> cEventMouseUp_vtable{};
}
