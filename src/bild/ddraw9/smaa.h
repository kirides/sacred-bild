#pragma once

struct IDirect3DDevice7;

// SMAA 1x on the Direct3D 9 backend's render target (smaa.hlsl): the game has no antialiasing of its own (its
// FSAA_FILTER option only pulls sprite texture coordinates in so bilinear filtering doesn't bleed at their edges). Run
// after the world view and before the UI, so the interface stays sharp: the frame so far is copied into a texture,
// edges are found by luma, blending weights computed with the area and search textures, and the neighborhood blend
// writes the result back into the render target.
namespace DDraw9::Smaa
{
    enum class Preset
    {
        Low,
        Medium,
        High,
    };

    // Antialiases what `device` (the backend's Direct3D 7 device) has drawn into its render target so far. False if
    // nothing was done (not the backend's device, no shader model 3, a resource could not be created).
    bool apply(IDirect3DDevice7* device, Preset preset);
}
