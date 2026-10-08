#pragma once
#include "game/sacred_addr.h"

#include <cstddef>
#include <cstdint>

struct IDirect3DDevice7;
struct IDirectDraw7;
struct IDirectDrawSurface7;

// Rendering: the DirectDraw 7 / Direct3D 7 driver, textures, the quad batcher and the render flags.
namespace Sacred
{
    // The DirectDraw7 + Direct3D7 wrapper (one global instance; identical in the English and German builds).
    struct dxDriver7
    {
        uint8_t _00[0x1C];
        uint16_t height;
        uint8_t _1e[2];
        uint16_t width;
        uint8_t _22[0x90 - 0x22];
        int32_t windowed;           // 1 = fullscreen, else windowed
        int32_t bpp;
        uint8_t _98[0xA8 - 0x98];
        // Screenshots: startCapture (ENG 00648660, thiscall (frames)) sets these; while framesLeft is set, flip locks
        // the back buffer and calls Addr::captureScreenshot.
        int32_t captureFramesLeft;
        int32_t captureFramesTaken;
        uint8_t _b0[0xB4 - 0xB0];
        IDirectDraw7* ddraw;
        uint8_t _b8[0xBC - 0xB8];
        IDirectDrawSurface7* back;  // the 3D render target
        uint8_t _c0[0xCC - 0xC0];
        IDirect3DDevice7* device;
    };
    static_assert(offsetof(dxDriver7, height) == 0x1C && offsetof(dxDriver7, width) == 0x20);
    static_assert(offsetof(dxDriver7, windowed) == 0x90 && offsetof(dxDriver7, bpp) == 0x94);
    static_assert(offsetof(dxDriver7, captureFramesLeft) == 0xA8 && offsetof(dxDriver7, captureFramesTaken) == 0xAC);
    static_assert(offsetof(dxDriver7, ddraw) == 0xB4 && offsetof(dxDriver7, back) == 0xBC);
    static_assert(offsetof(dxDriver7, device) == 0xCC);

    // A texture of the texture manager.
    struct cTexture
    {
        uint8_t _00[0x14];
        IDirectDrawSurface7* surface;   // nullptr: no texture
    };
    static_assert(offsetof(cTexture, surface) == 0x14);

    // The texture manager (one instance): loads textures on use, evicts least recently used ones above a budget.
    // initApp computes the budget from GlobalMemoryStatus and the reported video memory (min 32 MB).
    struct cTextureManager
    {
        uint8_t _00[0xC001C];
        uint32_t usedBytes;
        uint32_t budgetBytes;

        // nullptr before initApp made it.
        static cTextureManager* instance() { return *Addr::g_pTextureManager; }

        // The texture by handle: stamps it as used and loads it if it isn't.
        cTexture* get(uint32_t handle) { return Addr::cTextureManager_get(this, handle, 0); }
    };
    static_assert(offsetof(cTextureManager, usedBytes) == 0xC001C && offsetof(cTextureManager, budgetBytes) == 0xC0020);

    // The ground's quad batcher (cWorldView::quadBatcher): ground and layer quads collected until the texture changes,
    // then drawn by flush with one DrawIndexedPrimitive. add culls a quad (4 vertices) against the screen, appends it
    // and flushes first when full; setTexture sets the stage 0 texture (flushes on a change, stage 1 left alone).
    struct cQuadBatcher
    {
        static constexpr uint32_t fvf = 0x244;  // XYZRHW, diffuse, two texture coordinate sets: 36 bytes
        static constexpr uint32_t vertexSize = 36;

        uint8_t vertices[0x5460];
        uint16_t indices[(0x5910 - 0x5460) / 2];
        uint32_t vertCount;
        uint32_t indexCount;
        uint32_t flushes;           // statistics, counted by flush
        uint32_t triangles;
        uint32_t texture0;          // texture manager handles; texture1 0: stage 1 is left as it is
        uint32_t texture1;

        void flush(IDirect3DDevice7* device) { Addr::cQuadBatcher_flush(this, device); }
    };
    static_assert(offsetof(cQuadBatcher, indices) == 0x5460 && offsetof(cQuadBatcher, vertCount) == 0x5910);
    static_assert(offsetof(cQuadBatcher, indexCount) == 0x5914 && offsetof(cQuadBatcher, flushes) == 0x5918);
    static_assert(offsetof(cQuadBatcher, triangles) == 0x591C && offsetof(cQuadBatcher, texture0) == 0x5920);
    static_assert(offsetof(cQuadBatcher, texture1) == 0x5924);

    // The render flags (the 8-byte object at ENG 00CD7A7C, made on first use; see RE_NOTES "Draw calls and
    // batching"). drawTileLayers switches flag 0x2000 between its two-texture (off) and one-texture (on) blend layer
    // passes.
    struct RenderFlags
    {
        static constexpr uint32_t oneTextureLayers = 0x2000;

        static RenderFlags* instance() { return Addr::renderFlags_instance(); }

        // Nothing when unchanged, else applies it to the device.
        void set(uint32_t flag, bool on) { Addr::renderFlags_set(this, flag, on ? 1 : 0); }
    };
}
