#pragma once

// [Render] BatchSprites: the sprite batcher (cWorldView + 0x91AEC; water tiles and both object passes) flushes when
// the texture changes, which is nearly every sprite: SetTexture and DrawIndexedPrimitive through the device proxy
// each time, ~1,400 flushes per frame at 2560x1440. Its texture-change and end-of-pass flushes are replaced by ones
// that look the texture up as the game does and hand texture and quads to the batcher in one call (as BatchGround
// does for the ground). The flush inside its add (a full buffer, rare) stays the game's.
namespace SpriteQuads
{
    // Queues the hooks in the caller's Patch transaction.
    void install();
}
