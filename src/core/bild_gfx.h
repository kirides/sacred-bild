#pragma once
// SacredBild's 2D drawing for Sacred Gear (and anything else drawing over the game): a small C API that ddraw.dll
// exports as one versioned function table, so Gear's UI (RmlUi's geometry, pictures, rectangles) is recorded straight
// on SacredBild's renderer instead of going through Direct3D 7 calls, the device proxy and the Direct3D 7 emulation.
//
//   const SacredBild_Gfx* gfx = SacredBild_GfxGet(SACREDBILD_GFX_VERSION);   // GetProcAddress(ddraw.dll, ...)
//
// gfx is null where SacredBild's renderer can't take the draws (the Direct3D 9 backend, a chain-loaded ddraw, an older
// ddraw.dll): the caller keeps drawing with Direct3D 7. The table only grows; `size` says how much of it a ddraw.dll has.
//
// Resources (textures, meshes) can be made and released on any thread; a released one may still be drawn by what was
// recorded before. Drawing is begin(device) .. end() on the thread that draws the game's frame, with the device the game
// draws with; the draws are recorded in order with the game's own (end() and flush() draw what is pending, so call
// flush() before drawing anything with the device in between).
//
// A drawn rectangle (4 vertices) gets the fixes the device proxy gives the game's UI rectangles: texture coordinates
// fitted to the screen's pixels, clipped to a confined frame.
//
// Coordinates are the UI canvas's units (1024 x 768 laid out, as Gear's commands): SacredBild maps them to the screen
// the way it maps the game's UI (the canvas scale and placement, the UI frame pushed with SacredBild_UiPushFrame, its
// confinement), after the transform set with setTransform.
#include <cstdint>

struct IDirect3DDevice7;
struct IDirectDrawSurface7;

#define SACREDBILD_GFX_VERSION 1u

extern "C"
{
    // A vertex: position in canvas units, texture coordinates, color 0xAARRGGBB (premultiplied with blend
    // SACREDBILD_GFX_PREMULTIPLIED).
    struct SacredBild_GfxVertex
    {
        float x, y;
        float u, v;
        uint32_t argb;
    };

    enum
    {
        // Texture formats: 32-bit 0xAARRGGBB pixels; BC1 / BC2 / BC3 (DXT1 / DXT3 / DXT5) blocks, width and height
        // multiples of 4, pitch the bytes of a row of blocks.
        SACREDBILD_GFX_ARGB8 = 1,
        SACREDBILD_GFX_BC1 = 2,
        SACREDBILD_GFX_BC2 = 3,
        SACREDBILD_GFX_BC3 = 4,

        // Blending: source alpha (color * a + dest * (1 - a)), or premultiplied colors (color + dest * (1 - a)).
        SACREDBILD_GFX_ALPHA = 0,
        SACREDBILD_GFX_PREMULTIPLIED = 1,

        // Texture filtering (clamped at the edges either way).
        SACREDBILD_GFX_LINEAR = 0,
        SACREDBILD_GFX_POINT = 1,

        // SacredBild_GfxMeshDraw::flags.
        SACREDBILD_GFX_FLIP_U = 1,          // u -> 1 - u
        SACREDBILD_GFX_FLIP_V = 2,          // v -> 1 - v
        SACREDBILD_GFX_SWAP_UV = 4,         // u and v swapped (before the flips): an image turned by 90 degrees
        SACREDBILD_GFX_WRAP = 8,            // coordinates past 0..1 repeat the image inside its rectangle (atlas tiles)
        SACREDBILD_GFX_STRAIGHT = 16,       // the vertex colors are premultiplied, the texture is not: colors divided by alpha
        SACREDBILD_GFX_SNAP = 32,           // positions rounded to whole screen pixels (sharp at UI scales that are not 1)
    };

    // How drawMeshEx places a mesh: vertex (x, y) -> transform (setTransform) of (x + this.x, y + this.y), times
    // `scale` (mesh units per canvas unit, e.g. 1 / RmlUi's dp ratio), then the canvas mapping. The texture coordinates
    // (after the flags' flips) span the image's rectangle uv0 .. uv1 in the texture.
    struct SacredBild_GfxMeshDraw
    {
        float x, y;
        float scale;
        uint32_t texture;
        float u0, v0, u1, v1;
        uint32_t flags;
    };

    struct SacredBild_Gfx
    {
        uint32_t version;   // SACREDBILD_GFX_VERSION of the ddraw.dll
        uint32_t size;      // sizeof the table it fills

        // A texture of `width` x `height` from `pixels` (rows `pitch` bytes apart), 0 on failure. Id 0 is "no texture":
        // drawn as white (the vertex colors alone).
        uint32_t(__cdecl* textureCreate)(int width, int height, uint32_t format, const void* pixels, int pitch);
        // Replaces a rectangle of an ARGB8 texture (font atlases). 0 on failure.
        int(__cdecl* textureUpdate)(uint32_t texture, int x, int y, int width, int height, const void* pixels, int pitch);
        void(__cdecl* textureRelease)(uint32_t texture);

        // Geometry kept for drawing many times (RmlUi's compiled geometry): triangles, 0 on failure.
        uint32_t(__cdecl* meshCreate)(const SacredBild_GfxVertex* vertices, int vertexCount, const int* indices, int indexCount);
        void(__cdecl* meshRelease)(uint32_t mesh);

        // Starts drawing with `device` (the game's IDirect3DDevice7, as passed to the UI's drawing): 0 if SacredBild
        // can't draw with it now (then draw with the device). Each begin starts with no scissor, no transform, alpha
        // blending and linear filtering.
        int(__cdecl* begin)(IDirect3DDevice7* device);
        // Draws what is pending and ends drawing.
        void(__cdecl* end)();
        // Draws what is pending (before drawing anything else with the device).
        void(__cdecl* flush)();

        // Only the inside of this rectangle (canvas units, not transformed) is drawn; `enable` 0: everything.
        void(__cdecl* setScissor)(int enable, float x, float y, float width, float height);
        // A 4x4 transform applied to the vertices (x, y, 0, 1) before the canvas mapping, column-major (RmlUi's
        // Matrix4f layout: element (row r, column c) at [c * 4 + r]); null: none.
        void(__cdecl* setTransform)(const float* matrix);
        // Blending (SACREDBILD_GFX_ALPHA / _PREMULTIPLIED) and filtering (SACREDBILD_GFX_LINEAR / _POINT).
        void(__cdecl* setState)(uint32_t blend, uint32_t filter);

        // A mesh moved by (x, y), with `texture` (0: none).
        void(__cdecl* drawMesh)(uint32_t mesh, float x, float y, uint32_t texture);
        // Triangles given now; `indices` null: the vertices taken three at a time.
        void(__cdecl* draw)(const SacredBild_GfxVertex* vertices, int vertexCount, const int* indices, int indexCount,
            uint32_t texture);

        // A texture id for one of the device's surfaces (a game texture, a picture made with the device's DirectDraw),
        // valid until end(); 0 if it isn't one.
        uint32_t(__cdecl* surfaceTexture)(IDirectDrawSurface7* surface);

        // A mesh placed and textured as `draw` says. The mesh's vertices stay with SacredBild: nothing is computed per
        // vertex on the CPU, and the draws of one texture, state and scissor share one pipeline state.
        void(__cdecl* drawMeshEx)(uint32_t mesh, const SacredBild_GfxMeshDraw* draw);
    };

    // The table for `version` (SACREDBILD_GFX_VERSION), null if this ddraw.dll can't draw for others now. Exported by
    // ddraw.dll (exports.def).
    const SacredBild_Gfx* __cdecl SacredBild_GfxGet(uint32_t version);
}
