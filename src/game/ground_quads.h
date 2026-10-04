#pragma once

// [Render] BatchGround: the ground's quad batcher (cWorldView + 0x86890) flushes whenever the texture changes, which on
// the ground is nearly every quad: two SetTexture calls and a DrawIndexedPrimitive through the device proxy each time,
// ~7,000 flushes per frame zoomed out at 2560x1440. Its flush is replaced by one that looks the textures up as the
// game does and hands textures and quads to the batcher in one call.
namespace GroundQuads
{
    // Queues the hook in the caller's Patch transaction.
    void install();
}
