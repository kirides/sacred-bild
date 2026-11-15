#pragma once
#include "sacred/address.h"

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
}
