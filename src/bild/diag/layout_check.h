#pragma once

// Checks that the game's object layouts are the ones SacredBild's structs (src/bild/sacred) describe, from the game's own
// code: every world view field SacredBild reads or writes at a fixed offset must appear as a displacement in a
// function of the game that uses it. A modified exe (Sacred ReBorn moved the water tile array, for one) that moved a
// field fails its check, and the features that use the field stay off instead of reading or writing the wrong memory.
namespace Sacred::Layout
{
    // Runs the checks and logs what failed; call once after resolveAddresses().
    void check();

    // The fields of the ground rows (row walk, ground mesh, water list): rowLength, rowEdgeLeft / Right, rowEven / Odd,
    // sectors, layeredTileCount, waterTileCount, quadBatcher.
    bool worldRows();
    // pickLock, pickBegin, pickEnd (the pick list).
    bool worldPick();
    // zoom.
    bool worldZoom();
}
