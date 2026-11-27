#pragma once
#include "ui/ui_canvas.h"

#include <windows.h>

#include <cstdint>

// The device proxy's fixes for pretransformed UI rectangles (4 vertices, axis-aligned, texture coordinates following one
// axis each; anything else is left alone), shared with bild_gfx.h's draws. `verts`: 4 vertices `stride` bytes apart,
// x and y first, the first texture coordinate set at byte `texOffset`.
namespace UiQuads
{
    // Clipped to the confined frame's rect (`place`); false if nothing is left or the quad can't be clipped.
    bool clip(uint8_t* verts, UINT stride, UINT texOffset, UINT texCount, const UiCanvas::Placement& place);
    // Texture coordinates moved so each pixel samples its texel's middle and the edges stay inside the image's texels
    // (a `w` x `h` texture; `linear`: bilinear sampling).
    void fitTexels(uint8_t* verts, UINT stride, UINT texOffset, float w, float h, bool linear);
}
