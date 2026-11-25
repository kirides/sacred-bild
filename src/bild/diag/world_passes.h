#pragma once

// [Debug] D3DStats: splits the world view's time, device calls and draws by pass (tile rows, ground, layers, water,
// the object passes, 3D models), so the slow part is measured before it is replaced. Only installed with D3DStats:
// the quad batcher flush runs thousands of times per frame.
namespace WorldPasses
{
    // Opens its own Patch transaction: renderTileRow is detoured by Resolution as well.
    void install();
}
