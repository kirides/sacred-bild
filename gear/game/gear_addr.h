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
}
