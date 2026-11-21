#pragma once
#include <cstdint>

// Potions and the teleporter list (docs/GearApi.md: event `item.use`, gear.world.open_teleporter).
//
// A drunk potion runs cPotion3D::receiveEvent (ENG 0x5CAA60, thiscall (item, message*)): the message's word +4 is 0x10 and +8 the
// drinker's object id, and the item's type (+0x10) picks the effect; the handler marks a used-up potion with flag 0x8000000 at
// item +0x14 and the caller (0x54AA80) takes it out of the pack. The hook asks the mods first (`item.use`): a handler may keep the
// engine's effect or answer 'consume' (the potion is used up, no effect) or 'keep' (no effect, the potion stays).
//
// The list of portals the hero knows is opened by a message to the kernel's bus, as the script command Teleporter does (0x4AE040):
// a UI message (vtable 0x890DD0, `DEFAULT_BUSYDLG`, word +8 = 2, +0xC the sub-command, +0x10 the hero's portal mask at hero
// +0x578).
namespace ItemUse
{
    bool install();

    // Whether some mod handles `item.use` (the hook passes through while not).
    void setActive(bool active);

    // Sends the message that opens the teleporter list for the local hero; `sub` is 5 or 6 as in the script command (see the docs).
    bool openTeleporter(int sub, const char*& error);

    // The portals the local hero knows (hero +0x578, one bit per portal, as the script command Teleporter sets them); `set` writes it
    // (single player: the engine's own setter also announces the change to the other players).
    bool portals(uint32_t& mask, const uint32_t* set, const char*& error);
}
