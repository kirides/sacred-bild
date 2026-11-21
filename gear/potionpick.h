#pragma once
#include <cstdint>

// The potion picker (docs/GearApi.md: event `potion.pick`).
//
// ENG 0x43B1A0 (stdcall (mode, flag, tier), ret 0xC) chooses the item type of a random potion: the tier is 5 (the healing potions) with 60%, else a
// random tier in [flag ? 2 : 0, 4]; a `tier` argument 0 to 5 replaces that draw. A table of six rows (ENG 0x9499B4: the item types small, medium and
// large of each tier) gives the type: `mode` 0 takes the small one, 1 small or medium, 2 medium or large, anything else (the difficulty is passed as the
// mode) the large one. Callers: the merchants (the stock of a merchant's first visit 0x6DB676, a reopened merchant 0x6DB766, the window's refresh
// 0x6DB814, two helpers 0x6D9DD0 and 0x6DA183) and the loot of dying creatures (0x534756, 0x55A3B7, 0x55A4D4, 0x48646D). The hook asks the mods first (`potion.pick`):
// a handler may answer with another item type.
namespace PotionPick
{
    bool install();

    // Whether some mod handles `potion.pick` (the hook passes through while not).
    void setActive(bool active);
}
