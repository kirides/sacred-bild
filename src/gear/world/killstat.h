#pragma once
#include <cstdint>

// The kill statistics record (docs/GearApi.md: event `kill.recorded`).
//
// When a creature dies and its killer is the local hero, or a creature the local hero owns (a minion, a summon: the killer's owner,
// +0x251, is the hero), the death handlers (ENG 0x52E590, two sites: 0x534A5C for the killing blow, 0x5364C7 for the death event of
// the local player) call ENG 0x424250 (thiscall on the statistics block, ret 0x44, 17 stack words): it counts the kill (a per-id
// counter in 0x300 records of 16 bytes at the block's start, and the total at +0x56F0) and keeps the best values. The hook runs
// the engine's function and then tells the mods. Sacred ReBorn puts its "2% of every combo per kill" code in the same function.
namespace KillStat
{
    bool install();

    // Whether some mod handles `kill.recorded` (the hook passes through while not).
    void setActive(bool active);
}
