#pragma once

struct IDirect3DDevice7;

// [Render] SMAA on the NVRHI backend: SMAA 1x (shaders/smaa.hlsl) over what the device has drawn into its render target so
// far, from FrameHooks after the world view and before the interface, as the Direct3D 9 backend's (ddraw9/smaa.h). The
// frame is copied into a texture, edges are found by luma, blending weights computed with SMAA's area and search
// textures, and the neighborhood blend writes the result back into the render target.
namespace DDrawRhi::Smaa
{
    // `preset` 0 low, 1 medium, 2 high. False if nothing was done (not this backend's device, a resource failed).
    bool apply(IDirect3DDevice7* device, int preset);
}
