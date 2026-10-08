#pragma once

struct IDirect3DDevice7;

namespace Sacred
{
    struct cWorldView;
}

// [Render] GroundMesh: the ground's tiles and blend layers kept in vertex buffers instead of being rebuilt and drawn
// tile by tile every frame.
//
// Every frame the game walks the visible tiles, builds a pretransformed quad per tile (renderTileRow), draws them
// through its quad batcher, and then looks up and draws every tile's blend layers (drawTileLayers): ~4,000 base
// quads and ~7,000 layer quads at 2560x1440 zoomed out, each through the batcher. A tile's quad depends only on the
// tile (texture cell, corner heights, corner light) and on the camera, which moves the whole ground at once. So each
// loaded sector (64x64 tiles) gets vertex buffers in its own isometric space, filled as its tiles come into view
// (the game's row walk still visits them: it collects the objects), and the ground is drawn by a vertex shader that
// places a sector's buffers where the game would have put its quads: a few draws per sector and pass.
//
// The ground textures (256x256 sheets) are copied into atlas pages of their own, so a sector's quads of one pass share
// one texture binding. The game's own ground draws are skipped (its quad batcher's add and texture calls during the
// row walk, the layer pass); the water tiles, objects and everything else still go the game's way. Changed tiles (the
// fields a quad is made of) are noticed when the walk visits them, and their sector is built again.
namespace GroundMesh
{
    // Before the Resolution hooks: the row hook must see the rows as clipped to the loaded sectors.
    void install();
    bool active();
    // From cWorldView0::render, before the original (the batching scope is open).
    void beginFrame(Sacred::cWorldView* view, IDirect3DDevice7* device);
}
